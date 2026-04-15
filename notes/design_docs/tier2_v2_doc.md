# Tier-2 via the WasmEdge LLVM Frontend

**Date:** 2026-04-15
**Branch:** `tier2`
**Supersedes:** the `ir_emit_llvm` → LLVM IR text path documented in `tier2_doc.md`.

---

## Why this rewrite

The previous tier-2 pipeline took tier-1's serialized dstogov/ir graph, ran
it through `ir_emit_llvm` to get LLVM IR *text*, re-parsed that text with
`LLVMParseIR`, rewrote intra-batch calls, and fed it to ORC LLJIT. That path
carried three persistent costs:

1. **19 tracked bugs** against `ir_emit_llvm` and its supporting passes
   (PHI encoding, MERGE width, SCCP shape, etc.), most of which were
   single-kernel miscompiles masquerading as codegen bugs.
2. **Inherited parser limits** from `ir_load` — the most recent blocker
   being a hard cap of 255 inputs on MERGE nodes that kicks in on
   moderately branchy kernels.
3. **Duplicated lowering.** WasmEdge already owns a mature Wasm → LLVM
   lowering in `lib/llvm/compiler.cpp` (the AOT frontend). Tier-2 was
   re-implementing the same job, badly.

The goal of the rewrite was to **replace the "IR text → LLVM module" step
with a direct call into `WasmEdge::LLVM::Compiler::compile(AST::Module)`**,
so tier-2 lowers Wasm bytecode through the same path AOT uses. Everything
else — call-count prologue, background worker, atomic FuncTable swap, per-
module dedup, hot-function batching with depth-1 callees — is preserved.

The tiered-JIT philosophy is also preserved: tier-1 still runs everything,
tier-2 still only promotes a **selective subset** per tier-up event (hot
head + direct scalar-only callees, max 12). Whole-module recompilation
remains AOT's job.

---

## High-level pipeline

```
Wasm bytecode
  │
  ▼
Loader → AST::Module  (shared_ptr kept alive by IRJitEnvCache)
  │
  ├──► IR JIT tier-1 (unchanged) — fast baseline for every function
  │        │
  │        ▼
  │   per-call counter; on threshold: jit_tier_up_notify(funcIdx)
  │
  └──► Tier2Manager::enqueue(funcIdx, shared_ptr<AST::Module>, shared_ptr<FuncTable>)
              │
              ▼ (background worker thread)
         workerLoop():
           1. filter: is the hot function scalar-promotable?
           2. walk AST instrs of hot function → collect direct Call targets
           3. filter callees the same way, cap batch at 12
           4. Tier2Compiler::compileBatch(batch, fullModule, OptLevel=2)
              │
              ▼
           atomic FuncTable[idx] = fwd_thunk_ptr  for each batch member
```

`compileBatch` is the piece that changed end-to-end. Everything around it
(queue, dedup, swap, shutdown handling) was reshaped to carry
`shared_ptr<AST::Module>` instead of an IR text snapshot but is otherwise
the same design as `tier2_doc.md` describes.

---

## `compileBatch`: the rewritten core

```
compileBatch(BatchIdx, Mod, OptLevel)
  │
  ├─► synthesizeMiniModule(Mod, BatchSet)
  │     - deep-copy Mod (Type/Import/Func/Table/Mem/Global/Elem/Data/Export
  │       sections copied verbatim — funcIdx space is preserved)
  │     - for each defined function NOT in BatchSet:
  │         clear locals + body; emit "push default-typed constants; end"
  │         (i32.const 0 / i64.const 0 / f32.const 0 / f64.const 0)
  │     - batch members keep their real bodies
  │     - setIsValidated(true)   // parent was already validated
  │
  ├─► LLVM::Compiler(tier2Conf).compile(Mini) → LLVM::Data
  │     tier2Conf:
  │       OptLevel   = O0     ◄── CRITICAL: see "Why O0 at the frontend"
  │       Interruptible      = false  (no gas / interrupt checks)
  │       GenericBinary      = false  (tune for host)
  │       InstrCounting      = false
  │       CostMeasuring      = false
  │       TimeMeasuring      = false
  │
  ├─► Post-process the llvm::Module:
  │     for each funcIdx in BatchSet:
  │         emitFwdThunk(LLMod, funcIdx, funcType)       // tier-1 ABI entry
  │     for each funcIdx referenced by a batch body but NOT in BatchSet:
  │         emitT1ThunkInPlace(LLMod, funcIdx, funcType)  // tier-2 → tier-1 bridge
  │
  ├─► LLVMRunPasses(LLMod, "default<O2>", TM, opts)
  │     ▲ own the opt pipeline — runs *after* stub rewrites so opt sees
  │       real cross-tier bridges, not folded `ret i32 0` stubs
  │
  ├─► Hand to ORC LLJIT:
  │     LLVMOrcCreateLLJIT
  │     LLVMOrcAbsoluteSymbols: wasmedge_tier2_get_exec_ctx,
  │                             wasmedge_tier2_get_jit_env,
  │                             wasmedge_tier2_trace_thunk
  │     addLLVMIRModule
  │     lookup("intrinsics") → store &Executor::Intrinsics into the
  │                             mutable external global slot
  │     for each funcIdx in BatchSet:
  │         lookup(f<idx>_fwd_thunk) → native ptr for FuncTable swap
  │
  └─► Return vector<pair<funcIdx, void*>> to the worker.
```

---

## Mini-module synthesis

`synthesizeMiniModule(Src, BatchSet, ImportFuncNum)` produces a fresh
`AST::Module` that validates and parses through the frontend identically
to the parent, *except* for non-batch bodies.

**Why not just delete non-batch functions?** Because `call <funcIdx>` uses
module-wide indices and the batch bodies still reference them. We need a
function slot at every original index so the frontend's call lowering
resolves correctly.

**Why not `unreachable; end` for the stubs?** First attempt did. The LLVM
frontend then marks those functions `noreturn`, and batch bodies that call
them fold their control flow into `unreachable` — the batch body stops
executing at the call site. Switched to type-matched default returns
instead (`iN.const 0 ; end` or `fN.const 0 ; end`). These validate
trivially (the returned constants match each func's declared rets) and
produce LLVM bodies that look like `ret iN 0`. They *also* look very
foldable to opt, which is the hazard that drove the O0-then-O2 split
below.

**v128 / reftype stubs** fall back to `unreachable; end` — but this only
happens for non-batch defined functions that carry non-scalar signatures,
and the batch filter excludes calls into them by construction, so the
stubs are dead and noreturn is harmless.

Validation is skipped by setting `Mini.setIsValidated(true)` directly.
The parent was validated, the stubs validate trivially, and
`LLVM::Compiler::compile()` only checks the flag.

---

## Why O0 at the frontend, O2 after post-processing

**This is the single most important correctness invariant in this
pipeline. Getting it wrong produces silent wrong-code bugs that look like
memory corruption in kernels that use `memcpy` or any shared helper.**

`LLVM::Compiler::compile()` runs a full PassBuilder pipeline at the
configured `OptLevel` *before* it returns. If we hand it a mini-module
whose non-batch stubs are `ret iN 0`, opt at O2 cheerfully:

1. infers `readnone willreturn` on every stub,
2. inlines them into their call sites,
3. DCEs the call site and everything derived from its return value,
4. propagates constants through the caller.

By the time `emitT1ThunkInPlace` runs and rewrites the stub function body
into a real tier-2 → tier-1 dispatcher, **the call sites in batch bodies
no longer exist** — they were inlined and folded away. The rewrite only
helps callers that *will be* compiled later; for the batch bodies already
in the module, it is a no-op.

The concrete symptom this caused: in shootout-sieve, the musl `__fwritex`
(hot, in-batch) calls `memcpy` (non-batch, stub). O2-at-frontend saw
`memcpy = ret i32 0`, inferred readnone, and eliminated the call. The
batch body still advanced `f->wpos += len` afterwards, but the destination
bytes were never written. stdout contained stale memory that happened to
match the first `printf`'s source rodata — kernel output diverged from
golden.

The fix is structural, not a workaround:

```cpp
Conf.getCompilerConfigure().setOptimizationLevel(O0);
LC.compile(Mini);               // frontend does lowering + verify only
emitFwdThunk(...)  for batch;   // add cross-tier entries
emitT1ThunkInPlace(...) for non-batch callees;  // rewrite stubs
LLVMRunPasses(LLMod, "default<O2>", TM, opts);  // our own opt pipeline
```

At the O2 run, opt sees a module where every call site out of the batch
points at a t1_thunk with real side effects (FuncTable load, indirect
call, args-buffer materialization). It can still inline t1_thunks where
profitable — and does — but it can never DCE the call, because the
dispatch through `wasmedge_tier2_get_jit_env` is externally visible.

Batch members themselves are optimized equivalently to a normal
`Compiler::compile(..., O2)`. All inlining, GVN, loop opts, vectorization,
etc. happen at step 4 (the `LLVMRunPasses` call) — the only difference is
ordering relative to the stub rewrites.

### Why not prevent opt from seeing stubs in the first place?

Considered alternatives:

- **Mark stubs `optnone` / `noinline` in the mini-module.** Would work if
  the frontend gave us a hook; it does not, short of a Configure flag
  that doesn't exist. We'd be patching the frontend.
- **Run the frontend with `optnone`-equivalent opt level but keep O2
  elsewhere.** The Configure knob is a single enum — there is no
  per-function override.
- **Emit non-batch defined functions as declarations only.** The
  frontend's call lowering consults the `FunctionSection` / call graph to
  emit the right LLVM function type and linkage; making non-batch
  functions extern at the AST level breaks its invariants. Rejected as
  too invasive.
- **Rewrite stub bodies *before* `LC.compile(Mini)`** e.g. directly in
  the AST. We cannot emit the thunk body in Wasm — it needs raw pointer
  loads, inline helpers, and ABI glue that Wasm cannot express.

Compile at O0 then run opt ourselves is the least invasive option and
keeps all the ABI-glue knowledge on our side of the boundary instead of
leaking into the frontend.

---

## ABI bridging: two thunk flavors

Tier-1 (dstogov/ir) and the WasmEdge LLVM frontend use incompatible calling
conventions and context-pointer layouts. Two small adapters bridge them:

### `f<i>_fwd_thunk` — tier-1 → tier-2 (goes into FuncTable)

Entry point that tier-1 call sites land on after the atomic FuncTable
swap.

```
ret f<i>_fwd_thunk(void *env, uint64_t *args) {           // tier-1 ABI
  exec_ctx = wasmedge_tier2_get_exec_ctx();               // thread-local
  p0 = trunc  i64 args[0]  to i32       // for i32 params
  p1 = bitcast i64 args[1] to double     // for f64 params
  ...
  rv = call f<i>(exec_ctx, p0, p1, ...) // SysV, frontend ABI
  return tier1-wire(rv)                  // zext i32→i64, passthrough others
}
```

- `wasmedge_tier2_get_exec_ctx` is an absolute symbol bound to
  `Executor::getThreadLocalExecutionContextPtr` — the same TLS slot the
  frontend populates for AOT code. Using AOT's own context avoids
  reinventing the Memories/Globals/Intrinsics layout.
- Args come in as 8-byte slots (tier-1's scratch buffer convention).
  Each param type has its own load+cast sequence matching the frontend's
  expectations.
- Return value is remarshaled to tier-1's wire format:
  - void → `i64 0`
  - i32  → `zext i64`
  - i64  → passthrough
  - f32/f64 → passthrough (tier-1 returns the same wire type for floats)

### `f<i>_t1_thunk` — tier-2 → tier-1 (in-place rewrite of non-batch stub)

The function `f<i>` already exists in the LLVM module with the stub body
generated by the frontend. We do not add a new symbol — instead we
`LLVMDeleteBasicBlock` the stub body and emit a fresh entry block *on the
same Value*. Every call site in batch bodies that referenced `@f<i>`
picks up the new body automatically because LLVM references are by Value
pointer, not by name.

```
ret f<i>(ExecCtxPtrTy exec_ctx, <params...>) {
  args = alloca [NumParams x i64]
  for each wasm param k:
    args[k] = marshal(param[k])          // zext / bitcast to i64 slot
  env = wasmedge_tier2_get_jit_env()     // per-module JitExecEnv* (TLS)
  table = *env                           // FuncTable is at offset 0
  target = table[<i>]                    // tier-1 native ptr (or another
                                         // fwd_thunk from a later tier-2 batch)
  rv = call target(env, args)            // tier-1 ABI
  return unmarshal(rv)
}
```

Before rewriting, we strip any LLVM attributes the opt-at-frontend-or-
compiler might have inferred from the old constant-return body —
`readnone`, `readonly`, `memory`, `willreturn`, `mustprogress`,
`nofree`, `norecurse`, `cold`, `noinline`, `nounwind`, `noreturn`. None
of these hold for the dispatch body, and leaving them in place would let
callers continue to assume the function is pure.

The `in-place rewrite of the Value` property is what makes the whole
design work. Alternative designs (emit `f<i>_t1_thunk` as a new symbol
and patch call sites) require walking every use of `@f<i>` and
`RAUW`ing, which is more fragile and doesn't play well with the LLVM
inliner if we ever want to inline thunks later (which we do — the O2 run
after rewriting happily inlines short t1_thunks into their callers).

---

## Runtime helpers

Three extern-C helpers are bound into each tier-2 JIT dylib as ORC
absolute symbols:

| Symbol | Address | Called from | Purpose |
|---|---|---|---|
| `wasmedge_tier2_get_exec_ctx` | `Executor::getThreadLocalExecutionContextPtr` | fwd_thunk entry | Returns the TLS `ExecutionContextStruct*` that AOT lowering expects. Populated by the outer `SavedThreadLocal` in `enterFunction`. |
| `wasmedge_tier2_get_jit_env` | `ir_jit_engine.cpp` TLS accessor | t1_thunk entry | Returns the per-module `JitExecEnv*` set up by `IRJitEngine::invoke`. Its first field is `FuncTable`, which the t1_thunk indexes into. |
| `wasmedge_tier2_trace_thunk` | `tier2_compiler.cpp` | fwd_thunk entry, gated by `WASMEDGE_TIER2_TRACE_FUNC=<idx>` | Observability: prints `(func_idx, a0..a3)` and dumps `mem[a0..a0+min(a1,32)]` for scalar-only debugging of fwd_thunks. |

The `intrinsics` external global that the frontend emits (see
`lib/llvm/compiler.cpp` around line 5981) is resolved by a post-add
`LLVMOrcLLJITLookup("intrinsics")` — the LLJIT returns the address of
the global's storage slot, and we write `&Executor::Intrinsics` into it.
This makes `call_indirect` / imported-call dispatch fall through to the
same `Intrinsics` table AOT uses, including `proxyCallIndirect`.

---

## Promotion scope (MVP)

`Tier2Manager::isPromotable(...)` gates both the hot head and its
direct callees:

- Imports: **not promotable** (handled by frontend intrinsics path).
- Return arity > 1: skipped (multi-return marshaling not implemented).
- Any param or return that is not `i32`/`i64`/`f32`/`f64`: skipped
  (v128, reference types, externref).

Empirically on sightglass the scalar-only filter covers most hot code —
musl helpers, arithmetic kernels, the shootout suite, fib/sieve. Things
it leaves on tier-1: simd-heavy kernels, a few kernels with multi-return
helpers. Expanding scope to v128 is a follow-up, not a blocker.

Batch size is capped at **12** (hot head + up to 11 direct callees).
Direct calls only — transitive callees wait for their own tier-up events.

---

## File layout

### Changed

| File | Role |
|---|---|
| `include/executor/executor.h` | `IRJitEnvCache` gains `std::shared_ptr<const AST::Module> FullModule`. |
| `lib/executor/instantiate/module.cpp` | Stash a `shared_ptr<AST::Module>` into `IRJitEnvCache` at IR JIT setup. |
| `lib/executor/helper.cpp` | `jit_tier_up_notify` forwards the stashed `shared_ptr<AST::Module>` into `Tier2Manager::enqueue`. No more `ModuleFuncMap` construction. |
| `include/vm/tier2_manager.h` / `lib/vm/tier2_manager.cpp` | `Request = {funcIdx, shared_ptr<AST::Module>, shared_ptr<FuncTable>}`. Worker walks AST instrs for direct callees and calls `Tier2Compiler::compileBatch`. |
| `include/vm/tier2_compiler.h` / `lib/vm/tier2_compiler.cpp` | **Full rewrite.** Mini-module synthesis, fwd/t1 thunk emitters, O0-then-O2 pipeline, ORC plumbing. |
| `lib/vm/CMakeLists.txt` | Links `wasmedgeLLVM` into the tier-2 target. |

### Untouched on purpose

- `lib/vm/ir_builder.cpp` — tier-1 codegen unchanged; fwd_thunks make
  tier-2 entries ABI-compatible with tier-1 FuncTable dispatch, so no
  edits to the tier-1 side.
- `lib/vm/ir_jit_engine.cpp` — tier-1 engine unchanged (except it still
  exposes `wasmedge_tier2_get_jit_env` as before).
- `thirdparty/ir/*` — no changes. The 19 open `ir_emit_llvm` bugs no
  longer affect tier-2 because tier-2 no longer uses that path.
- `lib/llvm/compiler.cpp` — frontend used as-is. No hooks carved into
  the AOT pipeline for tier-2.

---

## Observability

| Env var | Effect |
|---|---|
| `WASMEDGE_TIER2_ENABLE=1` | Turn on background tier-2 recompilation. |
| `WASMEDGE_TIER2_THRESHOLD=N` | Call-count threshold for tier-up (tier-1 side). |
| `WASMEDGE_TIER2_LOOP_THRESHOLD=N` | Loop-back-edge threshold for tier-up. |
| `WASMEDGE_TIER2_MAX_COMPILE=N` | Debug: stop after N successful compilations. |
| `WASMEDGE_TIER2_DUMP_IR=<dir>` | Write post-processed, post-opt LLVM modules to `<dir>/tier2_f<headIdx>.ll`. |
| `WASMEDGE_TIER2_TRACE_FUNC=<idx>` | Dump entry trace and memory snapshot for `f<idx>`'s fwd_thunk. |

Log lines to look for:

- `tier2: starting batch compile for func N with M function(s)` — worker
  picked up a request and filtered a batch.
- `tier2: rewrote K non-batch stubs as t1_thunks` — post-processing ran
  (debug level; normal if K>0 for any non-trivial kernel).
- `tier2: step-5 ORC done for func N (emitted M thunks)` — ORC finished,
  fwd_thunks looked up.
- `tier2: upgraded func N → tier-2 (0x...)` — FuncTable atomically
  swapped; tier-1 callers now land on tier-2.

---

## Verification

Baseline suite: full sightglass under IR JIT O2 with tier-2 enabled, 32
kernels (spidermonkey and tinygo excluded per standing instructions).

```shell
cd build && for wasm in ../test/ir/testdata/sightglass/*.wasm; do
  kernel="$(basename "$wasm" .wasm)"
  case "$kernel" in spidermonkey|tinygo) continue;; esac
  WASMEDGE_SIGHTGLASS_KERNEL="$kernel" \
  WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
  WASMEDGE_SIGHTGLASS_QUICK=1 \
  WASMEDGE_IR_JIT_OPT_LEVEL=2 \
  WASMEDGE_TIER2_ENABLE=1 \
  WASMEDGE_TIER2_THRESHOLD=10 \
  WASMEDGE_TIER2_LOOP_THRESHOLD=5 \
  stdbuf -oL timeout 60 \
    ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
done > /tmp/tier2-all.log 2>&1
grep -iE 'dumped|failed|error|mismatch' /tmp/tier2-all.log || echo "All passed"
```

Current status: **32 / 32 kernels pass**, at least one `tier2: upgraded`
line per non-trivial kernel, no core dumps.

Bug histories closed by the rewrite:

- All `ir_emit_llvm` miscompiles: path deleted.
- `MERGE > 255 inputs` parse failure: path deleted.
- `IR_ADDR` null emission: path deleted.
- `ir_load` PHI shape mismatches: path deleted.
- Silent stdout corruption in shootout-sieve / shootout-ctype: fixed by
  the O0-then-O2 split above (the last wrong-code bug encountered before
  verification passed).

---

## Known limits & follow-ups

1. **Scalar-only scope.** v128 and reference types fall back to tier-1.
   Sightglass coverage is still high, but any simd-heavy kernel will
   stay on tier-1 indefinitely.
2. **Validation cost.** `synthesizeMiniModule` sets `setIsValidated(true)`
   to avoid a re-validate on every tier-up. If we ever start generating
   stubs that don't trivially type-check, this shortcut must go.
3. **No de-tiering.** FuncTable swaps are one-way. ORC LLJITs are
   retained for the lifetime of the `Tier2Compiler` because the code
   returned to the FuncTable lives inside them. This is fine for server
   workloads; a long-running embedder that wants to evict cold tier-2
   code needs a refcounted JIT cache.
4. **Mini-module copy cost.** `AST::Module(Src)` deep-copies every
   section. Cheap compared to an LLVM compile but measurable on huge
   modules. A shared-section / COW variant would help if copy time ever
   shows up in tier-2 latency profiles.
5. **`intrinsics` global binding is post-add, not absolute.** We lookup
   the global slot after `addLLVMIRModule` and write `&Executor::Intrinsics`
   into it. This matches AOT's own runtime loader pattern. If the
   frontend ever switches the global to `constant` linkage, the write
   would segfault — add a build-time assertion if that ever drifts.
6. **Scope-filter hit rate is not measured.** We should emit a counter
   for "tier-up requests skipped due to non-scalar signature" to drive
   the v128 scope-expansion decision.

---

## Acknowledgements / context for the next reader

- The plan this was built from lives at
  `/home/tommy/.claude/plans/starry-tickling-parasol.md`. It covers the
  step-by-step build-up (AST preservation → manager plumbing → mini-
  module synthesis → fwd_thunk → intrinsics → t1_thunk → verification).
  The plan's risk section called out items 1 (`intrinsics` symbol vs.
  global) and 6 (non-batch symbol fixup collisions) — both landed as
  concrete issues during bring-up and are resolved as described above.
- The superseded IR-text path's rationale and history are in
  `tier2_doc.md`. That file is accurate up to the `ir_emit_llvm` era;
  anything that calls `Tier2Loader` / `loadIRText` / `emitLLVMIR` /
  `rewriteIntraBatchCalls` / `stripTierUpPrologue` / `SharedEmitLoader`
  belongs to the old path and is slated for deletion in step 8 of the
  migration plan.
