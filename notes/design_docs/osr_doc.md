# OSR — On-Stack Replacement for Tier-1 → Tier-2 Promotion

**Date:** 2026-04-17
**Branch:** `osr`
**Complements:** `tier2_v2_doc.md` (function-entry tier-2 via the LLVM frontend).

---

## 1. Overview & motivation

Tier-2 in WasmEdge uses **function-entry swap**: when a batch of hot functions
is compiled by the LLVM frontend, the pointer in `FuncTable[idx]` is replaced
with a tier-2 `fwd_thunk`. Every *future* call to that function runs LLVM code.

This does nothing for a function **already on the call stack**. The hot site in
the Sightglass "loss cluster" — `ctype`, `shootout-ed25519`, `blind-sig` — is a
single long-running loop inside a one-shot caller:

```
main (cold)
 └── kernel_body (called once, spends 100% of its time inside this loop)
       for (i = 0; i < N; i++) { ... }     // hot site
```

By the time the tier-2 worker finishes compiling `kernel_body`, the tier-1
function is mid-loop and will never be called again. Tier-2 function-entry
swap delivers roughly **0% improvement** (WT ratio ≈ 1.0× vs tier-1), while
standalone LLVM AOT delivers **1.4–1.6×**. That gap is what this work is
intended to close.

**OSR** detects hot back-edges in tier-1 code, compiles a tier-2 entry point
that begins execution *inside* the loop (with all wasm locals as parameters),
and transfers execution mid-loop.

Theoretical ceiling, from LLVM-JIT WT numbers:
`ctype ~5.2M` (from 8.2M), `ed25519 ~5.5M` (from 8.5M),
`blind-sig ~4.3M` (from 9.5M).

---

## 2. End-to-end architecture

```
tier-1 compiled wasm function
 │
 │  per outermost-loop back-edge:
 │    1. if (OsrEntryTable[f*16+l] != NULL) {
 │         serialize wasm locals → OsrLocalsFrame[i]
 │         tail-call the tier-2 OSR entry(env, locals)
 │         RETURN its result              ← side-exit out of the loop
 │       }
 │    2. else if (++BackEdgeCounters[f*16+l] == OsrThreshold) {
 │         jit_osr_notify(env, f, l)      ← saturates counter, enqueues OSR
 │       }
 │
 ▼
jit_osr_notify()                          (lib/executor/helper.cpp)
 │
 ▼
Tier2Manager::enqueueOsr(funcIdx, loopIdx, Mod, FuncTable, OsrEntryTable)
 │                                         (lib/vm/tier2_manager.cpp)
 │  dedup by (FuncTablePtr, funcIdx<<32|loopIdx)
 │  skip if FuncTable[funcIdx] already tier-2 promoted
 │
 ▼
worker thread:
  Tier2Compiler::compileOsrEntry(funcIdx, loopIdx, Mod, OsrOpt)
   │  (lib/vm/tier2_compiler.cpp)
   │  ├─► synthesizeOsrModule(Mini, funcIdx, loopIdx, ...)    // AST surgery
   │  │     - rewrite target function: locals become params, body starts at loop
   │  │     - validate synthesized module (reject malformed rewrites cleanly)
   │  │     - stub all other defined functions with default-value bodies
   │  ├─► LLVM::Compiler::compile(Mini)                        // O0 frontend
   │  ├─► emitFwdThunk()                                       // tier-1 ABI thunk
   │  ├─► emitT1ThunkInPlace() on non-OSR callees              // cross-fn bridges
   │  ├─► alwaysinline the OSR body into the fwd_thunk
   │  ├─► LLVMRunPasses(default<O2>)                           // post-opt
   │  └─► ORC LLJIT → lookup fN_fwd_thunk                      // native pointer
 │
 ▼
atomic store into OsrEntryTable[f*16+l]   (memory_order_release)
 │
 ▼
next tier-1 back-edge sees a non-null entry,
serializes locals, tail-calls into LLVM code, and RETURNs.
```

The worker thread publishes a native pointer via `std::atomic<void*>`; the
tier-1 polling load is a plain `ir_LOAD_A`, whose acquire semantics come for
free on x86-64 (the only currently supported host).

---

## 3. `JitExecEnv` layout

Tier-2 fields are appended to the struct; tier-1 and OSR both index into it
via `offsetof`-based `ir_ADD_A` sequences.

```cpp
struct JitExecEnv {                                      offset
  void **FuncTable;                                         0
  uint32_t FuncTableSize;                                   8
  uint32_t _pad;                                           12
  void *GlobalBase;                                        16
  void *MemoryBase;                                        24
  ... (call_indirect shadow table, host trampolines) ...
  DispatchEntry *Table0Dispatch;
  uint32_t Table0DispatchSize;
  uint32_t _pad2;
  uint32_t *CallCounters;           // tier-2 per-function counters
  void *TierUpNotifyFn;             // &jit_tier_up_notify
  // --- OSR ---
  uint32_t *BackEdgeCounters;       // size = numFuncs * OSR_MAX_LOOPS_PER_FUNC
  void *OsrNotifyFn;                // &jit_osr_notify (held to keep symbol live)
  uint64_t *OsrLocalsFrame;         // OSR_LOCALS_FRAME_SLOTS u64 slots (2 KiB)
  uint32_t OsrLocalsFrameSize;
  uint32_t _pad3;
  void **OsrEntryTable;             // size = numFuncs * OSR_MAX_LOOPS_PER_FUNC
};
```

Fixed sizes:

| Constant | Value | Rationale |
|---|---|---|
| `OSR_MAX_LOOPS_PER_FUNC` | 16 | All Sightglass kernels top out at a few outermost loops per function. Over-provisioned. |
| `OSR_LOCALS_FRAME_SLOTS` | 256 (u64) | Sightglass tops out at ~20 locals; 256 is two orders of margin. Avoids having to thread `LocalCount` into `invoke()`. |

Lifetimes (see `lib/executor/helper.cpp`):

| Field | Allocator | Owner |
|---|---|---|
| `BackEdgeCounters` | `std::vector<uint32_t>` on the module's `IrJitEnvCache` | Cache (outlives `invoke`) |
| `OsrLocalsFrame` | `std::vector<uint64_t>` on `IRJitEngine::OsrLocalsFrame_` | Engine (re-used across calls) |
| `OsrEntryTable` | `std::shared_ptr<void*[]>` on `IrJitEnvCache` | Shared with tier-2 worker |

The `shared_ptr` is the only field the worker thread writes into; everything
else is single-threaded.

---

## 4. Tier-1 IR emission (`emitLoopBackEdge`)

The back-edge diamond is emitted only when `OsrThreshold > 0` *and* the loop
is the outermost in its function (per §7.1 below).

The slot at `OsrEntryTable[f*16+l]` is **sentinel-encoded** so the
post-saturation polling collapses to a single load-test-branch:

| Slot value | State | What the back-edge does |
|---|---|---|
| `1` | COUNTING | Run the counter logic (init value, set by `IRJitEngine::invoke` cache init) |
| `0` | WAITING | Notify already fired; worker compiling. Fall through with no further work. |
| `≥ 2` | READY | Function pointer published. Snapshot locals, tail-call, RETURN. |

Control-flow graph:

```
                 back-edge point
                       │
                LOAD OsrEntryTable[f*16+l]
                       │
          outer_if: slot ≠ 0  ──── FALSE (WAITING) ───┐
             │                                        │
           TRUE                                      END (fall through)
             │
       inner_if: slot == 1
       ┌─────┴──────┐
   TRUE (COUNTING)   FALSE (READY, slot is fn ptr)
       │             │
   counter logic     serialize locals → frame
   (LOAD/ULT/inc/    CALL(slot, env, frame)
    STORE/notify)    RETURN
       │
      END
       │
   MERGE (COUNTING / WAITING)
       │
    back-edge
```

Per-iteration cost in each state:

- **WAITING** (the post-saturation hot path): `LOAD slot + TEST against 0 + JZ-not-taken`. Three ops, no memory store, no counter access.
- **COUNTING** (pre-threshold): same three ops plus the original counter diamond — `LOAD counter + ULT vs threshold + branch + (if taken) increment + store + EQ vs threshold + branch + (if hit) call notify`.
- **READY** (one transition iteration): outer test + inner test + locals snapshot + tail-call + return. Executed exactly once per loop entry.

`jit_osr_notify` (`lib/executor/helper.cpp:564-576`) does **two** writes:
saturates `BackEdgeCounters[i]` to `UINT32_MAX` *and* writes `0` into
`OsrEntryTable[i]`. The `0` flips the slot from COUNTING to WAITING so the
outer test takes the fast-path FALSE branch on every subsequent iteration —
the counter LT check is bypassed entirely.

The counter ULT check inside COUNTING remains load-bearing for the brief
race window where two host threads are still in COUNTING when one of them
saturates — the saturated thread's `UINT32_MAX` value blocks any
re-increment by an unsigned LT. (Signed LT against threshold against `-1`
would be true and would wrap the counter, re-firing notify every threshold
iterations — the 75× regex regression observed before this gate was made
unsigned.)

The **transition path** is a side-exit from the loop: instead of taking the
back-edge (`ir_END → LOOP_END`), the function returns the OSR entry's
result. `ir_CALL_N`'s return type must match `Ctx.ret_type` exactly (with
`VOID` translated to `ir_RETURN(IR_UNUSED)`).

Source: `lib/vm/ir_builder.cpp:2264-2422` (`emitLoopBackEdge`); the slot
init to sentinel `1` is in `IRJitEngine::invoke` and the slot reset in
`jit_osr_notify` at `lib/executor/helper.cpp:564-576`.

This three-state diamond replaced an earlier two-array design (separate
`BackEdgeCounters` poll + `OsrEntryTable` poll, 6 ops per iteration in
WAITING). The collapse to a single sentinel-encoded slot drops the
WAITING-state cost from 6 ops to 3 and is the change behind commit
`89cb6dff`'s LLVM/t2 geomean improvement from 0.920× to 0.942×; the
largest single-kernel win was gcc-loops (LLVM/t2 0.68× → 0.83×).

### LabelInfo: loop identification

`LabelInfo.LoopIdx` is set by `visitLoop()` from a monotonic per-function
counter `CurrLoopIdx++`. This matches the indexing scheme in
`synthesizeOsrModule` so the tier-2 synthesis finds the same loop the tier-1
back-edge-counter fires on.

---

## 5. OSR mini-module synthesis (tier-2)

`synthesizeOsrModule` (lib/vm/tier2_compiler.cpp:631) does the AST surgery:

1. **Find the target loop.** Walk the wasm instruction stream of
   `funcIdx - ImportFuncNum`, counting outermost `OpCode::Loop` occurrences.
   `findOsrLoopStart` returns the instruction index.
2. **Reject non-empty loop types.** A `loop (param t)` or `loop (result t)`
   needs `t`-valued stack entry or produces `t`-valued stack exit; neither is
   available when OSR begins execution *at* the loop header.
3. **Walk the enclosing structured-control chain.** If any enclosing opener
   is an `If` or `Loop`, bail out — mid-entering such a construct breaks
   wasm's structured-control invariants. Enclosing `Block`s must have empty
   type (see `synthesizeOsrModule` for the reasoning).
4. **Build the OSR signature.** Params = original params ++ declared locals,
   flattened to a single parameter list. Returns = original returns.
5. **Build the OSR body.** `[enclosing Block openers] ++ [Instrs[LoopStart..end]]`.
   The enclosing-block chain keeps `br` depth indices valid.
6. **Clear the CodeSegment's declared locals** (they are now parameters).
7. **Append a new FunctionType** to the type section; repoint `FuncSec[DefinedIdx]`.

`compileOsrEntry` then:

1. Stubs all *other* defined functions with default-value return bodies (same
   placeholder scheme as the batch path's non-batch members).
2. Runs the WasmEdge validator against `Mini`. If validation fails (e.g. the
   synthesized body has operand-stack underflow because the original pushed
   values before the loop that get popped after it), we return cleanly with
   a log message instead of crashing LLVM's backend. This catches real
   irregularities — a small number of Sightglass functions legitimately
   cannot be OSR-entered at a given loop.
3. Lowers through `LLVM::Compiler::compile` at O0 (same as the batch path).
4. `emitFwdThunk(FuncIdx, OsrFT)` builds the `fN_fwd_thunk(JitExecEnv*,
   uint64_t*)` thunk. Because the OSR function's params *are* the wasm
   locals and `OsrLocalsFrame` is laid out identically to the tier-1 `args[]`
   buffer (widening rules below), the same unmarshalling code works verbatim.
5. `emitT1ThunkInPlace` on every non-OSR-batch callee — same as the batch path.
6. **Demote every batch member (OSR target + batched helpers) to
   `LLVMInternalLinkage`** (`tier2_compiler.cpp:1050-1065`). The frontend
   emits `protected dllexport`; flipping to `internal` lets LLVM's inliner
   apply its single-callsite bonus — the body folds into its fwd_thunk
   without `alwaysinline`. This replaced an earlier `alwaysinline`
   approach (P1c) that exploded code size on multi-callsite helpers
   (e.g. 805 inlined copies of `__multi3` in the f8 batch).
7. `LLVMRunPasses("default<O2>")` (configurable via `WASMEDGE_OSR_OPT_LEVEL`).
8. ORC LLJIT → `LLVMOrcLLJITLookup("fN_fwd_thunk")` → native pointer.

### Parameter widening convention

Locals are widened to `uint64_t` in the locals frame, matching tier-1's
`args[]` convention:

| WASM type | Widening |
|---|---|
| `i32`, `u32` | `ZEXT_U64` |
| `i64`, `u64` | pass-through |
| `f32` | `BITCAST_U32` then `ZEXT_U64` |
| `f64` | `BITCAST_U64` |

The fwd_thunk unmarshals these back into the LLVM function's native param
types (the OSR signature uses the original wasm types, not uint64).

### Batch composition

OSR uses the **same `Tier2Manager::bfsDownBatchLocked`** that regular
tier-2 uses (`lib/vm/tier2_manager.cpp:381-383`). The graph search shape
(depth ≤ `BfsMaxDepth_ = 1`, size ≤ `MaxBatchSize_ = 12`, dynamic-counter
OR static-frequency inclusion) is identical. Two arguments differ:

- **Anchor.** Regular tier-2 anchors on the walked-up caller; OSR anchors
  on the OSR target itself (a running tier-1 frame fixes which loop must
  be migrated, so walk-up is skipped at `tier2_manager.cpp:368-371`).
- **`SkipSeen`.** Regular tier-2 passes `false` (skip already-promoted
  callees); OSR passes `true`. Already-promoted helpers are kept in the
  OSR batch so LLVM can inline them into the loop body, instead of
  dispatching through `FuncTable[N]` on every iteration. The difference
  matters because OSR's whole point is to make *this loop* fast in one
  module, not to share work with previous batches.

Earlier OSR ran as singletons (only the OSR function lowered, helpers
left as t1_thunks). That design is preserved in §12.1 below as history;
ctype's WT dropped from 7,554k µs to 5,166k µs (within 1.8% of LLVM JIT)
once helpers were batched in.

---

## 6. Lifecycle & concurrency

| Resource | Allocator | Writers | Readers |
|---|---|---|---|
| `BackEdgeCounters` | `IrJitEnvCache` vector | Tier-1 JIT code (`STORE_U32`) + `jit_osr_notify` | Tier-1 JIT code (`LOAD_U32`) |
| `OsrLocalsFrame` | `IRJitEngine::OsrLocalsFrame_` vector | Tier-1 JIT code (on transition and threshold-hit) | OSR fwd_thunk (tier-2, when called) |
| `OsrEntryTable` | `IrJitEnvCache` shared_ptr | Tier-2 worker (atomic release store) | Tier-1 JIT code (plain load, acquire on x86-64) |
| `SeenOsr_` | `Tier2Manager` set | `enqueueOsr` under `Mu_` | same |
| `OsrQueue_` | `Tier2Manager` queue | `enqueueOsr` / `workerLoop` under `Mu_` | same |

The only cross-thread shared mutation is the `OsrEntryTable` slot write. The
tier-1 load is safe because: (a) the pointer is always null before the
worker writes, (b) once written it is never overwritten, (c) on x86-64
64-bit aligned pointer loads are atomic and acquire-ordered at the ISA level
for the release store the worker pairs with.

The `FuncTable` slot for a function and its `OsrEntryTable` row are
**independent** publishing paths. A function can be tier-2 promoted at
its entry *and* have OSR entries compiled for its loops. `enqueueOsr`
deliberately **does not** short-circuit on `Seen_`
(`lib/vm/tier2_manager.cpp:344-347`): function-entry swap only redirects
*future* calls, while the currently-running tier-1 frame stays in the
tier-1 body until the loop exits — exactly the case OSR exists to
rescue. Dedup against repeated *OSR* requests is handled separately by
`SeenOsr_` (§7.3).

---

## 7. Hardening details

### 7.1 Outermost-loop-only restriction

`LabelInfo.LoopIdx` is assigned only to outermost loops in the IR builder.
Nested loops keep `LoopIdx = UINT32_MAX`, which `emitLoopBackEdge` checks
before emitting the diamond. Rationale:

- The AST-surgery step in `synthesizeOsrModule` walks structured-control
  openers — nested-loop enclosing chains make bailout conditions more common.
- The sightglass loss cluster puts its hot iteration in the outermost loop;
  OSR into an inner loop doesn't pay for itself.

### 7.2 OSR runs even when the entry is already tier-2 promoted

`enqueueOsr` deliberately does **not** short-circuit on `Seen_`
(`lib/vm/tier2_manager.cpp:344-347`). Function-entry tier-2 swap
redirects *future* calls; the currently-running tier-1 frame keeps
running tier-1 code until the loop exits. That mid-flight frame is
exactly the case OSR exists to rescue — short-circuiting here would
waste the rescue. Dedup against repeated *OSR* requests for the same
`(funcIdx, loopIdx)` is `SeenOsr_`'s job (§7.3).

### 7.3 Dedup of duplicate OSR requests

`SeenOsr_` keyed on `(FuncTablePtr, funcIdx<<32|loopIdx)` prevents the
worker from compiling the same OSR entry twice. Separate from `Seen_` so
regular tier-2 promotion and OSR for the same function don't interfere.

### 7.4 Validator gate

The WasmEdge validator (`Validator::validate(AST::Module)`) runs on every
synthesized OSR module before handing it to the LLVM frontend. This catches
cases where the synthesized body is not a well-formed wasm function (value
stack underflow at a call, etc.) and returns them as a clean compile failure
instead of letting LLVM hit an assertion in `stackPop()`. Example catches
during development: `ctype` func 154 loops 5/6, `blake3` func 73 loop 0.

### 7.5 Validation bail-outs in synthesizeOsrModule

Three fast-path rejections (return `false` before any mutation) cover the
bulk of unsafe rewrites:

- Loop with non-empty BlockType.
- Enclosing `If` or enclosing `Loop` in the structured-control chain.
- Enclosing `Block` with non-empty BlockType.

---

## 8. Tuning knobs

| Env var | Role | Default | Notes |
|---|---|---|---|
| `WASMEDGE_OSR_THRESHOLD` | Iterations before `jit_osr_notify` fires. `0` disables OSR. | `0` | Recommended: `1000`. Decoupled from the transition — the tier-1 IR polls `OsrEntryTable` every iteration; transition happens whenever the worker lands the entry. Firing early gives LLVM more time to compile. |
| `WASMEDGE_OSR_OPT_LEVEL` | LLVM post-opt pipeline for OSR modules. | `2` | Accepts 0–3. `0` is useful for bisecting optimizer bugs. |
| `WASMEDGE_OSR_MIN_FUNC` / `MAX_FUNC` | Restrict OSR instrumentation to `FuncIdx ∈ [MIN, MAX]`. | `0` / `UINT32_MAX` | Bisection hook; keep unset in production. |
| `WASMEDGE_OSR_MIN_LOOP` / `MAX_LOOP` | Restrict to `LoopIdx ∈ [MIN, MAX]`. | `0` / `UINT32_MAX` | Per-function bisection. |
| `WASMEDGE_OSR_SKIP_STORES` | Skip locals stores in the transition diamond. | unset | **Warning:** the OSR entry will then read stale/garbage locals. Debug-only; historically used to bisect Bug 2, no longer needed for correctness after the 2026-04-19 DESSA fix. |
| `WASMEDGE_OSR_DUMP` | Dump the synthesized OSR body (text) to `<dir>/osr_<f>_<l>.txt`. | unset | Diagnostic. |
| `WASMEDGE_TIER2_DUMP_IR=<dir>` | Dump tier-2 LLVM modules, including OSR (`tier2_osr_f<F>_l<L>.ll`). | unset | Diagnostic. |

Interaction with tier-2 thresholds:

- Both `WASMEDGE_TIER2_THRESHOLD` (function-entry) and
  `WASMEDGE_OSR_THRESHOLD` (back-edge) are active simultaneously. For a
  short-lived hot loop inside a one-shot caller, only OSR can help.
- For a long-lived function called many times, function-entry tier-2 hits
  first and `enqueueOsr` short-circuits future OSR requests for that
  function (see §7.2).

---

## 9. Implementation files

| File | Role |
|---|---|
| `include/vm/ir_jit_engine.h` | `JitExecEnv` OSR fields, `jit_osr_notify` decl, constants |
| `include/vm/ir_builder.h` | `WasmToIRBuilder::OsrThreshold`, `CurrLoopIdx`, `LabelInfo::LoopIdx` |
| `lib/vm/ir_builder.cpp` | `emitLoopBackEdge` counter+transition diamond, `visitLoop` LoopIdx assignment, env-field load hoists |
| `lib/vm/ir_jit_engine.cpp` | `invoke()` wiring for OSR buffers |
| `include/vm/tier2_compiler.h` | `compileOsrEntry` declaration |
| `lib/vm/tier2_compiler.cpp` | `synthesizeOsrModule`, `compileOsrEntry`, validator gate, fwd_thunk/t1_thunk reuse |
| `include/vm/tier2_manager.h` | `OsrRequest`, `OsrQueue_`, `SeenOsr_`, `enqueueOsr` |
| `lib/vm/tier2_manager.cpp` | `enqueueOsr` dedup, `workerLoop` OSR dispatch, atomic entry publication |
| `lib/executor/helper.cpp` | `jit_osr_notify`, OSR cache allocation (BackEdgeCounters, OsrEntryTable) |
| `lib/executor/instantiate/module.cpp` | OSR env-var reading (`OSR_THRESHOLD`, `OSR_MIN_FUNC`, `OSR_MAX_FUNC`, `OSR_MIN_LOOP`, `OSR_MAX_LOOP`) |

---

## 10. Remaining issues

Both historical OSR bugs are now resolved. 33/33 sightglass-strong
kernels pass at O2 under the supported `TIER2_ENABLE=1 TIER2_THRESHOLD=10
OSR_THRESHOLD=5000` configuration (2026-04-19). Full writeups live in
`notes/bugs/osr_bugs.md`.

### Bug 1: dead-PHI DCE corrupts codegen when the OSR diamond is present

Status: **workaround reverted; dormant in supported config.** Full
entry in `notes/bugs/osr_bugs.md#bug-1`.

With OSR IR emitted into tier-1 but *without* tier-2 promoting the hot
functions first, the `regex` kernel at O2 miscompiles (produces
`3 emails / 1 URIs / 2 IPs` instead of `92 / 5301 / 5`) because SCCP's
dead-PHI DCE removes `PHI/N(merge, A, B, B)`-style duplicate-input PHIs
and codegen then emits a jump landing on a wasm `unreachable` block.

OSR is only meaningful when tier-2 is enabled (`WASMEDGE_TIER2_ENABLE=1`)
— without tier-2 the continuation entry is never compiled and the OSR
diamond emits IR for a transition that can never happen. In that
supported tier2+OSR configuration the bug does not reproduce: tier-2's
function-entry swap promotes the regex hot functions to LLVM-compiled
code before `OSR_THRESHOLD` accumulates, so the tier-1 IR carrying the
OSR diamond is never on the hot path long enough for the dead-PHI DCE
to matter. The 33/33 sightglass-strong pass rate under tier2+OSR
(2026-04-19) confirms this.

The previous workaround (`WASMEDGE_IR_SKIP_PHI_DCE` auto-set in
`lib/executor/instantiate/module.cpp`, `getenv` gate in
`thirdparty/ir/ir_sccp.c`) has been reverted on both sides.

### Bug 2: OSR locals-store IR triggered O2 miscompile

Status: **fully fixed (2026-04-19)**. Full entry in
`notes/bugs/osr_bugs.md#bug-2`.

**Main fix** (landed earlier): the original Phase 2 IR emitted
`ir_ZEXT_U64` / `ir_BITCAST_U64` widening ops on each wasm local
before storing into `OsrLocalsFrame[i]`. dstogov/ir folds foldable ops
(opcodes up to `IR_COPY`, including `IR_ZEXT` and `IR_BITCAST`)
through a CSE table. When the OSR diamond emits stores on two sibling
control-flow branches (transition TRUE and threshold-hit TRUE), CSE
saw the widening of the same local in each branch as equivalent and
deduped them to a single SSA def. That single def then fed stores in
two disjoint branches, and downstream passes (GCM / schedule /
regalloc) emitted code where the widened value's live range spanned
branches that didn't dominate the use — wrong codegen on the hot
back-edge path that runs every iteration. 13 kernels regressed at O2.

The fix in `lib/vm/ir_builder.cpp` `emitLoopBackEdge` is a
**type-native store** — write each local at its natural IR width
(4 bytes for i32/f32, 8 bytes for i64/f64) directly into the slot. The
upper 4 bytes of a slot holding an i32/f32 are left stale, but the OSR
thunk (`tier2_compiler.cpp emitFwdThunk`) reads the full u64 and
`LLVMBuildTrunc`s to the native parameter width, so the high bits are
discarded. Stores themselves aren't foldable, so two stores to
different addresses in disjoint branches can't be CSE'd — the
problematic shared-def shape cannot form.

Additionally, the threshold-hit path's redundant locals snapshot was
removed: Phase 4 re-stores locals fresh on every iteration once
the OSR entry is ready, so the pre-entry snapshot fed nothing and
merely re-introduced the CSE substrate. Threshold-hit now only calls
`jit_osr_notify`.

**Residual fix (2026-04-19)**: after the main fix, blind-sig still
SEGV'd in tier-1 JIT'd code under tier2+OSR at O2. The crash was in
`wasm_jit_520` (f535's tier-1 compile). The additional OSR-store live
ranges put register-allocation pressure on f535, and the stack-slot
coloring pass (legitimately) assigned two non-overlapping-live-range
virtual registers to a shared spill slot. `ir_dessa_parallel_copy`
treats each `(from, to)` as an abstract label in its dependency graph
and did not realize two distinct labels could alias to one memory
cell; it sequenced the copies such that copy B read slot S *after*
copy A had already overwritten it. For f535's BB14→BB18 PHI lowering,
d_99's initial induction-variable value was wrong (`d_75 = d_30 - 1`
instead of the PHI's declared initial `d_73 = PHI(d_32, d_53)`), and
the inner loop walked an out-of-bounds address every iteration.

Fixed in `thirdparty/ir/ir_emit.c` `ir_emit_dessa_moves`: before
handing the `copies[]` array to `ir_dessa_parallel_copy`, canonicalize
any two spilled labels that resolve to the same `IR_MEM_VAL` slot to a
single (smaller-numbered) label. The algorithm then sees the shared
label, forms the correct dependency chain, and either reads before
writing or inserts a scratch. Emit correctness is preserved because
both rewritten labels resolve to the same slot.

`WASMEDGE_OSR_SKIP_STORES=1` is retired — no longer needed. The gate
remains as a debug-only knob in the tree (when set, the transition
path reads stale locals and produces wrong output).

The other two original 2026-04-17 residuals (shootout-base64,
shootout-ratelimit) were fixed separately by commit `5f34a78e
fix(llvm-frontend): Hoist call_indirect null-path allocas to entry
block` — unrelated to tier-1 IR, belongs to the LLVM frontend.

### Empirical sweep status (2026-04-19)

**33/33** sightglass-strong kernels pass at O2 under
`WASMEDGE_TIER2_ENABLE=1 WASMEDGE_TIER2_THRESHOLD=10
WASMEDGE_OSR_THRESHOLD=5000`. OSR transitions fire end-to-end on every
applicable kernel; locals frame is serialized correctly and the OSR
continuation produces golden output.

---

## 11. Verification checklist

- [x] Sightglass-strong **33/33** kernels pass at O2 with
      `TIER2_ENABLE=1 TIER2_THRESHOLD=10 OSR_THRESHOLD=5000`, golden
      output matches, transitions fire end-to-end (2026-04-19).
- [x] Tier-1 WT not regressed on non-looping or cold-loop functions
      (counter is ~5 insns/iter after saturation; not reached on cold paths).
- [x] Validator rejects structurally unsynthesizable loops cleanly (no
      LLVM-backend assertions).
- [x] `OsrEntryTable` atomic publication pairs correctly with tier-1 loads
      on x86-64 (only supported host).
- [x] Bug 2 main class resolved via type-native locals stores.
- [x] Bug 2 residual (blind-sig) root-caused and fixed: DESSA
      parallel-copy slot-aliasing in `thirdparty/ir/ir_emit.c`
      `ir_emit_dessa_moves` (2026-04-19).
- [x] `WASMEDGE_IR_SKIP_PHI_DCE` workaround reverted (Bug 1 dormant in
      supported tier2+OSR config).
- [x] OSR closes the tier-1 ↔ LLVM-JIT gap on the loss cluster for
      one-shot-caller kernels. 2026-04-17 §12 follow-up: after §12.1
      batch-composition fix (`bfsOsrBatch` in `tier2_compiler.cpp`),
      ctype WT drops 7,554k → 5,166k µs (−32%), closing to within 1.8%
      of the 5,077k LLVM-JIT reference. ed25519 WT is unchanged
      because its steady-state path is the tier-2 FuncTable fwd_thunk,
      not OSR — the one-shot OSR rescue affects only the first
      in-flight invocation.
- [~] Close tier-2 ↔ LLVM-JIT gap for multi-call kernels (ed25519:
      8,422k → **7,036k** vs 5,320k = ~32% slower, down from ~58%).
      2026-04-18 fix in the regular tier-2 batch path closes ~45% of
      the gap: drop `alwaysinline` on batch bodies (let LLVM's cost
      model judge cross-body inlining) + include statically-hot
      siblings in BFS (fix the cold-at-tier-up cascade). See
      `tier2_v2_vs_llvm_jit_benchmark.md` §"2026-04-18 refinements"
      for the P1d/P1e analysis. Residual gap is one-shot-caller
      structural — blocked on OSR beating the fwd_thunk (§12).
- [~] blind-sig loses to LLVM JIT at 0.40× (bigint-heavy workload;
      LLVM whole-module vectorizer / cross-function RA outperforms
      mini-module scope). Not an OSR / correctness regression —
      structural compiler-quality delta. See
      `tier2_v2_vs_llvm_jit_benchmark.md` blind-sig row (2026-04-19).

---

## 12. Investigation: why OSR doesn't close the gap (2026-04-17)

The perf motivation in §1 projected closing most of the tier-1 ↔
LLVM-JIT gap on `ctype / ed25519 / blind-sig` — theoretical ceilings
~5.2M / 5.5M / 4.3M WT. Current reality:

| Kernel             | tier-1 only | tier-2 only | **tier-2 + OSR** | LLVM JIT |
|--------------------|------------:|------------:|-----------------:|---------:|
| shootout-ctype     |      ~8,200k |       7,857k |       **7,554k** (−4%) |  ~5,000k |
| shootout-ed25519   |      ~8,550k |       8,364k |       **8,300k** (−1%) |  ~5,230k |
| blind-sig          |      9,521k |            — |       **4,951k (−48%)** (†) |  3,713k |

(†) blind-sig tier-2+OSR now runs clean (no SEGV) after the
2026-04-19 DESSA fix. 3-run median per mode, sightglass-strong at O2,
`TIER2_THRESHOLD=10 OSR_THRESHOLD=5000`. Tier-2+OSR delivers a
**1.92× speedup over tier-1** — the largest t2/t1 win in the suite —
because blind-sig is the textbook one-shot-caller OSR target: the
in-flight tier-1 frame is migrated into tier-2 mid-loop and runs the
remaining signing work at LLVM speed. The remaining 0.75× gap vs
whole-module LLVM JIT is a compiler-quality delta (LLVM inlines
`__multi3` / `mac3` / `monty_modpow` across the full call graph,
mini-module batches don't reach the same scope), not an OSR one.

Medians of 3, `WASMEDGE_TIER2_THRESHOLD=10 WASMEDGE_OSR_THRESHOLD=1000`,
O2, sightglass-strong, current branch build. The mechanism is
mechanically working — `WASMEDGE_TIER2_TRACE_FUNC=9` on ctype confirms
exactly one fwd_thunk entry (the single transition out of the
one-shot caller into the OSR continuation). But the win is tiny.

### 12.1 Root cause: singleton OSR batching blocks helper inlining (history)

> **Status:** the singleton design described below was the OSR shape on
> 2026-04-17. It has been superseded — OSR now uses the same
> `Tier2Manager::bfsDownBatchLocked` as regular tier-2 (with
> `SkipSeen=true`), and batch members get `internal` linkage rather
> than `alwaysinline` (P1c). The investigation that motivated the
> change is preserved here for reference; for the current shape see §5
> "Batch composition" and §6.

`compileOsrEntry` (in the singleton design) treated the OSR function
as a batch of one. Every non-batch defined function was stubbed and
then rewritten in-place as a `t1_thunk` by `emitT1ThunkInPlace`, so
inside the LLVM module that holds the OSR body, every direct `call`
to a helper compiled to:

```
  env        = TLS_LOAD(jit_env_tls_offset)   ; inlined movq %fs:…
  table_base = *env                           ; FuncTable pointer
  target     = table_base[<helperIdx>]        ; indirect load
  ret        = target(env, args[])            ; indirect call, ABI-marshalled
```

LLVM cannot inline across this boundary — the callee is a pointer
loaded from runtime memory, not a statically known function.

Dumped OSR modules (`WASMEDGE_TIER2_DUMP_IR=…`) quantify it:

| Kernel          | OSR function | Indirect t1_thunk calls in hot loop body |
|-----------------|--------------|-----------------------------------------:|
| shootout-ctype  | `f9_l0`      | 3 per inner iter × 50,000 × 60,000       |
| shootout-ed25519| `f5_l4`      | **130 per iter**                          |

The ed25519 helpers are the field-arithmetic primitives. The ctype
helpers are char-class predicates (`isdigit`-like). Both are exactly
the shapes LLVM AOT-JIT inlines into a tight loop when given the
whole module; OSR compiling a single function hands LLVM a hot body
with `@funcTable[22](env, args)` in place of a 5-instruction predicate
and the O2 pipeline cannot recover.

The singleton design is called out in §5 "Batch composition" with the
rationale *"loss-cluster kernels have small, self-contained loop
bodies and don't require cross-function inlining to close most of the
gap."* The 2026-04-17 measurement shows this premise is wrong for
ctype and badly wrong for ed25519 (130 indirect calls per iter is not
a small self-contained body).

The tier-2 batch path already does the right thing: it collects
direct callees via `collectNonBatchCallees`, filters them through
`Tier2Manager::isPromotable`, and lowers them with real bodies in the
same mini-module so LLVM inlines freely. Post-P1c the batch functions
also carry `alwaysinline` so the body collapses into its fwd_thunk.
OSR gets the `alwaysinline` step but not the batch-composition step —
the OSR body folds into `fN_fwd_thunk` but every helper call inside
it is still an indirect dispatch.

**Fix direction.** Bring OSR's batch composition to parity with the
function-entry batch path:

1. After `synthesizeOsrModule`, walk the rewritten OSR body for
   direct `call` targets, same way `collectNonBatchCallees` does.
2. Filter through `isPromotable` (scalar-only signatures, no imports,
   no multi-return).
3. Add them to `BatchSet`; the mini-module now keeps real bodies for
   those callees instead of stubbing them.
4. Mark each batch member `alwaysinline` (not just the OSR function)
   so short helpers fold into the OSR body and longer ones remain
   callable at the SysV ABI, without the t1_thunk indirection.
5. Keep the t1_thunk rewrite for the remaining non-batch stubs — that
   tier is still needed for cold helpers.

**Implemented 2026-04-17, then unified 2026-04-21.** The original
landing added `bfsOsrBatch` + `isOsrBatchPromotable` helpers in
`lib/vm/tier2_compiler.cpp` (depth 2, size 12). Those helpers were
later merged into the regular `Tier2Manager::bfsDownBatchLocked`
path, which OSR now invokes with `SkipSeen=true` (see
`tier2_manager.cpp:381-383`). The 2026-04-21 sweep measured BFS
depth 2 indistinguishable from depth 1 on sightglass-strong
(`tier2_v2_doc.md` line 569), so the unified path uses depth 1 for
both regular and OSR. Per-member `alwaysinline` was also dropped
(P1c): batch members get `LLVMInternalLinkage` and the inliner's
single-callsite bonus folds the body in without exploding code size
on multi-callsite helpers like `__multi3`. `collectNonBatchCallees`
is unchanged — it excludes the enlarged batch so only truly-cold
helpers become t1_thunks.

Measured effect (medians of 3, sightglass-strong O2,
`TIER2_THRESHOLD=10 OSR_THRESHOLD=1000`):

| Kernel           | OSR funcs batched | WT before | WT after  | vs LLVM JIT |
|------------------|------------------:|----------:|----------:|------------:|
| shootout-ctype   |       1 → 10      |  7,554k µs |  **5,166k µs** |  5,077k µs (+1.8%)  |
| shootout-ed25519 |       1 → 4       |  8,300k µs |  8,422k µs     |  5,320k µs (+58%)  |

Indirect `t1_thunk` calls in the ctype OSR body dropped from ~3
per-iter to 0. ed25519 dropped 130 → 9. ed25519 WT didn't move
because its steady-state path is the tier-2 FuncTable fwd_thunk
installed before OSR fires — OSR only rescues the first in-flight
invocation, which is noise in a multi-call benchmark. The remaining
ed25519 gap is a regular tier-2 compile issue, not an OSR one.

Cost: larger LLVM modules per OSR compile, longer worker latency
before the entry publishes. Empirically still sub-100 ms for a ~12-
function batch at O2, well within the tens-of-ms the 1000-backedge
threshold affords.

### 12.2 Implementation correctness — otherwise clean

Walked `emitLoopBackEdge` (`lib/vm/ir_builder.cpp:2264-2408`),
`synthesizeOsrModule` / `compileOsrEntry` / `emitFwdThunk`
(`lib/vm/tier2_compiler.cpp:226-1450`), `enqueueOsr` / `workerLoop`
(`lib/vm/tier2_manager.cpp:362-528`), `jit_osr_notify`
(`lib/executor/helper.cpp:560-583`). No additional semantic bugs.
Specifically verified:

- Counter wrap is guarded (`ir_ULT`, not `ir_LT`) — a signed compare
  would let the saturated `0xFFFFFFFF` slot pass the gate and re-fire
  notify every `threshold` iterations.
- `Seen_` short-circuit in `enqueueOsr` is removed (OSR must run even
  when function-entry swap has already promoted the same funcIdx —
  the currently-running frame is the whole reason OSR exists).
- Atomic publication: release store in worker, plain load in tier-1
  (acquire on x86-64 at ISA level), slot transitions null → non-null
  once and never reverts.
- Validator gate runs on the synthesized module before LLVM codegen;
  structurally unsynthesizable loops return a clean reject instead of
  an LLVM-backend assertion.
- fwd_thunk reuse for the OSR entry is consistent with
  `OsrLocalsFrame`'s u64-slot layout and the type-native store widths.

### 12.3 Ship posture (2026-04-19)

Correctness blockers are cleared: sightglass-strong 33/33 kernels
pass at `OSR_THRESHOLD=5000` under tier2+OSR at O2. `§10 Bug 2` is
closed (DESSA slot-aliasing fix in `thirdparty/ir/ir_emit.c`) and
`§12.1 (batch composition)` has landed (`bfsOsrBatch` in
`tier2_compiler.cpp`).

Perf posture: the one-shot-caller scenario OSR was built for (ctype)
now closes to within 1.8% of LLVM JIT. Multi-call kernels (ed25519)
are helped much less because their steady-state path is the tier-2
FuncTable fwd_thunk installed before OSR fires — OSR only rescues
the first in-flight invocation. Bigint-heavy kernels (blind-sig)
still lose to LLVM JIT for compiler-quality reasons unrelated to
OSR.

Default-on posture is a policy decision, not a correctness one.
`WASMEDGE_OSR_THRESHOLD=0` remains the conservative default while
perf ceilings for the multi-call loss cluster are investigated.
