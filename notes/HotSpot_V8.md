# HotSpot and V8 Tier-Up Architectures — Comparison with the WasmEdge 2-Tier IR JIT

**Date:** 2026-04-20
**Scope:** Compare the tier-up machinery of the two most studied production JIT
engines — OpenJDK HotSpot and Chrome V8 — against the `tier-1 IR-JIT (dstogov/ir)
→ tier-2 mini-module LLVM` design documented in `notes/design_docs/tier2_v2_doc.md`
and `notes/design_docs/osr_doc.md`, and distil the architectural lessons that
actually apply.

Companion benchmark reference:
`notes/benchmarking/tier2_v2_vs_llvm_jit_benchmark.md` (2026-04-19 unification
sweep — `t2/t1` geomean 1.092×, `t2/LLVM` geomean 0.947×).

---

## 1. HotSpot (OpenJDK)

### 1.1 Tier structure

| Level | Engine | Profiling | Notes |
|---|---|---|---|
| 0 | Template interpreter | Invocation + back-edge counters | Instant start; per-bytecode machine stubs |
| 1 | C1 (client) | None | Trivial methods only (tiny/predictable) |
| 2 | C1 (client) | Invocation + back-edge | Brief stopgap tier |
| 3 | C1 (client) | **Full MDO** (profile-collecting) | The load-bearing middle tier |
| 4 | C2 (server) | *Consumes* MDO | Sea-of-Nodes SSA, heavyweight optimizer |

Typical trajectory is `0 → 3 → 4`. Levels 1 and 2 exist for trivial methods or
when the C2 queue is saturated.

### 1.2 Profile signal: the MDO

The **Method Data Object** is HotSpot's persistent profile record — the reason
tier 3 exists. It stores per-bci:

- Invocation and back-edge counters
- **Receiver-type histogram** at every virtual/interface call site (morphism
  class: monomorphic / bimorphic / polymorphic / megamorphic)
- **Branch-taken counters** (for conditional layout and unreachable-branch
  pruning)
- Null-check counters, array-store-type checks, arraylength observations

C2 reads MDO as codegen input and specializes against it. This handoff from a
profile-collecting lower tier to an optimizing upper tier is the architectural
spine of HotSpot.

### 1.3 OSR

OSR entries are keyed `(method, bci)`. C1/C2 generate a **distinct entry
point** whose parameters are the interpreter local array, not the method's
formal parameters. The interpreter frame is migrated into the JIT'd frame at
the loop header and execution resumes. Mechanically identical to what WasmEdge
does, with the `OsrEntryTable[f*16+l]` slot replaced by a `(method, bci)` map.

### 1.4 Deoptimization

Deopt is the enabler for speculation.

- Every C2 compile records a **ScopeDesc** per safepoint, mapping native frame
  state (regs, stack) back to JVM-visible state (locals, operand stack, `bci`).
- Speculative guards (`uncommon_trap`) branch to a deopt stub on mismatch. The
  stub rebuilds one or more interpreter frames from the ScopeDesc and
  resumes.
- **Not-entrant marking**: when a class loads that invalidates a CHA-based
  devirtualization, all dependent C2 methods are marked not-entrant. Active
  frames continue; new callers take the interpreter path. Sweeper eventually
  reclaims.

### 1.5 Optimization-enabling machinery

- **CHA (Class Hierarchy Analysis)**: global "is this virtual call effectively
  monomorphic *right now*?" query. If yes, devirtualize without a guard, track
  a dependency, deopt-on-class-load.
- **Speculative monomorphic/bimorphic inlining**: if MDO shows one (or two)
  receiver types dominating, inline those with a type guard; polymorphic/
  megamorphic sites fall back to vtable dispatch.
- **Escape analysis**: scalarize non-escaping allocations on the stack.
- **Range-check elimination** driven by profile and loop analysis.

### 1.6 Operational machinery

- Multiple compile threads — `-XX:CICompilerCount` (default 2–4), with C1 and
  C2 pools separate. Priority queue keyed on method hotness.
- **Code cache** with sweeper: not-entrant methods evicted, reclaiming native
  memory. Cold compiled methods can be swept too.
- Dynamic tier thresholds scaled by method size / call frequency.

---

## 2. V8 (Chromium / Node.js)

### 2.1 Tier structure (2024–2025 state)

| Tier | Engine | Role |
|---|---|---|
| 0 | **Ignition** interpreter | Register-based bytecode interpreter; writes feedback |
| 1 | **Sparkplug** (2021) | Non-optimizing baseline JIT; one-pass bytecode→machine code, no IR |
| 2 | **Maglev** (2023, Chrome M117) | Mid-tier optimizing JIT; CFG-SSA IR, fast to build |
| 3 | **TurboFan** | Full optimizing; Sea of Nodes; aggressive speculation |

Typical thresholds (approximate, tuned heuristically):

- Ignition → Sparkplug: effectively immediate (early in function life)
- Sparkplug → Maglev: ~500 invocations
- Maglev → TurboFan: ~6000 invocations

Each tier runs only *as long as it pays off*; cold functions stay in Ignition
forever. In-flight work: **Turbolev** / **Turboshaft** (2025) is moving V8
toward using Maglev's cheap CFG-SSA IR as TurboFan's frontend and a new
modular backend, suggesting the industry consensus that Sea-of-Nodes is too
expensive to build for most hot functions.

### 2.2 Profile signal: feedback vectors + ICs

Every function owns a **feedback vector** sized by feedback-slot count. Every
polymorphic operation (property load/store, call, arithmetic, `in`, `instanceof`)
has a slot. Ignition and Sparkplug write to it:

- **Inline Caches (ICs)**: each call site / property access progresses through
  uninitialized → monomorphic → polymorphic → megamorphic. Records
  receiver-Shape(s) seen and resolved targets.
- **Hidden classes / Shapes / Maps**: V8 synthesizes per-object structural
  types so ICs can key on "this receiver has the shape `{x:int, y:int}`" —
  this structural identity is the basis on which speculation works in dynamic
  JS.

Maglev and TurboFan consume the feedback vector as specialization input. The
feedback vector is *persistent* — it survives tier boundaries and deopts.

### 2.3 OSR

OSR fires at both tier boundaries (Sparkplug → Maglev, Maglev → TurboFan).
Same structural idea as HotSpot / WasmEdge: a loop-header entry point
receives interpreter locals.

### 2.4 Deoptimization — eager and lazy

- **Eager deopt**: optimized code itself finds a mismatch (`CheckMap` guard
  fires) and jumps to a deopt stub that rebuilds bytecode-interpreter state.
- **Lazy deopt**: another thread marks a function deopt-pending on a
  dependency invalidation; the next entry to that function takes the
  interpreter path. Active frames continue until they return.

Deopt is the single most important correctness-preserver in V8: every
speculation (shape guards, inlined call targets, fast-path arithmetic, elided
bounds checks) is "assume common case, deopt on miss."

### 2.5 Optimization-enabling machinery

- **Speculative inlining** across call sites based on feedback — with Shape
  guards.
- **Feedback-typed fast paths** for arithmetic (Smi vs HeapNumber), property
  loads (inline offset load vs megamorphic lookup), array element kinds.
- **Concurrent compilation** with priority — hot callees compile ahead of
  newly-warm ones; compile threads overlap with execution.

### 2.6 Operational machinery

- Code objects are **GC-managed** — no explicit sweeper; the GC sees compiled
  code as another object graph.
- Multiple background compile threads, priority-queued.
- Feedback vectors are allocated eagerly when a function is first seen.

---

## 3. Design comparison with WasmEdge's IR JIT

| Axis | HotSpot | V8 | **WasmEdge IR JIT** |
|---|---|---|---|
| # tiers | 5 (levels 0–4) | 4 (Ignition, Sparkplug, Maglev, TurboFan) | **2** (tier-1 IR JIT, tier-2 LLVM mini-module) |
| Bottom tier | Template interpreter (~free) | Ignition + Sparkplug (interp + baseline; ~free/cheap) | **dstogov/ir at O0–O2** — an already-optimizing JIT |
| Top tier | C2 Sea-of-Nodes | TurboFan Sea-of-Nodes | LLVM O2 via mini-module |
| Profile signal | Full MDO (types, branches, nulls, calls) | Feedback vectors + ICs | **Call counters + back-edge counters only** |
| Speculation | CHA devirtualization, type guards, range-check elim, escape analysis | Shape-guarded inlining, feedback-typed fast paths | **None** |
| Deopt | Yes → interpreter | Yes (eager+lazy) → bytecode interpreter | **None — FuncTable swap is one-way** |
| OSR | Yes, `(method, bci)` keyed | Yes, at every tier boundary | Yes, `(funcIdx, loopIdx)` keyed |
| Profile handoff across tiers | MDO (tier 3 → tier 4) | Feedback vector (persistent) | **Counter → batch decision; no signal handed to LLVM** |
| Inlining scope policy | Profile-driven InlineTree + size budget | Feedback-driven, bounded by budget | **Syntactic BFS depth 2, size 12** |
| Code cache mgmt | Sweeper, not-entrant eviction | GC-managed code objects | **Retained for process lifetime** |
| Compile workers | Pool (C1+C2 threads configurable) | Concurrent priority pool | **1 worker, OSR-priority then FIFO** |
| Transitions | Interp → C1 → (possibly) C2 | Ignition → Sparkplug → Maglev → TurboFan | **tier-1 → tier-2 direct** |

**Where the design fundamentally diverges.** Both HotSpot and V8 have a lower
tier whose primary purpose is to *collect profile data* that the upper tier
specializes against (`MDO` for HotSpot; `feedback vector + ICs` for V8). The
WasmEdge tier-1 collects only a single integer per function — enough to
decide *when* to tier up, nothing usable to decide *how* to compile. The
result: tier-2 sees exactly the same input AOT sees and cannot structurally
outperform AOT; at best it approaches it.

**Where the design legitimately simplifies.** Wasm is statically typed — no
Shape-polymorphism problem, no virtual dispatch through class hierarchies. A
large slice of what MDO and feedback vectors record is irrelevant in Wasm. The
meaningful residue is: `call_indirect` target distribution, branch-taken
frequencies, memory-alignment observations. None of those are collected
today.

---

## 4. Limitations of the current design

Ranked by how much they bound the perf envelope shown in the 2026-04-19
benchmark.

### 4.1 No profile beyond counters

The single biggest envelope-setter. The ed25519 0.72× and blind-sig 0.81×
losses vs LLVM JIT are both structurally explained by "LLVM sees the whole
module; the mini-module batch sees 12 functions." With even a basic `
call_indirect` target histogram, tier-2 could inline the dominant target and
recover most of that gap. Without any profile, tier-2's only lever is to
widen the batch — and widening the batch is expensive and hit-or-miss.

### 4.2 No deoptimization → no speculation

`FuncTable[idx] = tier2_fwd_thunk` is atomic but **one-way**. There is no
path back to tier-1 for an in-flight frame, and no path at all for a
compiled-in assumption that becomes false. This closes off:

- Monomorphic `call_indirect` inlining (needs "if target ≠ expected, trap")
- Aligned-memory speculation (`i32.load` where all observed runs were aligned)
- `br_table` hot-arm inlining with a fallback dispatch
- Eliminated bounds checks on provably-in-range-so-far indices

The entry-thunk work (tier2_v2 doc, P1g) is the clearest symptom — it pays a
`~15–25c thunk marshal` tax on every `call_indirect` precisely because
nothing safer than a direct ABI-compatible thunk is available without deopt.

### 4.3 Tier-1 is heavy; no cheaper tier below it

dstogov/ir at O2 runs GCM, SCCP, DESSA, coalescing, regalloc. Compared to
V8's Sparkplug (one-pass bytecode→machine code, no IR) or HotSpot's template
interpreter (free), this is several hundred microseconds per function on
first touch. Acceptable for server workloads, painful for cold-start
latency-sensitive embedders. There's no cheaper fallback.

### 4.4 Fixed batch geometry

`BfsMaxDepth_=1`, `MaxBatchSize_=12`. Syntactic cap, not a profile-driven
budget. The `bfsOsrBatch` work (osr_doc §12.1) showed that expanding scope on
ctype 1→10 drops WT 7,554k → 5,166k µs (−32%); ed25519 expanding 1→4 but
still 130 helpers/iter left unbatched only moves WT 8,300k → 8,422k — the
cap is the binding constraint. HotSpot's `InlineTree` and V8's inlining both
use `callsite_count / callee_size` as the priority and run out of **budget**,
not out of depth.

### 4.5 Static-frequency gate is a proxy for hotness

`static_freq >= 2 || CallCounters[c] != 0` is a two-gate heuristic that
approximates "frequent at IR-static time OR already measured warm." It can't
distinguish a called-twice-statically cold helper from a called-twice-in-IR
hot helper. HotSpot and V8 use real measured invocation counts per call
site.

### 4.6 No code cache eviction

`Tier2Compiler` retains ORC LLJITs for life (documented as follow-up 3).
Long-running embedders hosting many short-lived modules accumulate native
memory without bound. HotSpot's sweeper and V8's GC both solve this.

### 4.7 Single worker, naive priority

Two-queue FIFO (`OsrQueue_` drained first, then `Queue_`). Wide call graphs
— the `rust-html-rewriter` / `rust-json` / `hashset` family — queue up
behind their own batches. HotSpot runs multiple C2 threads; V8 uses a
priority concurrent pool. The benchmark table shows worker-backlog as a
secondary contributor to `rhr` and `rust-json` losses.

### 4.8 Scalar-only promotion filter

`i32 / i64 / f32 / f64` single-return only. v128 and reference types fall
through to tier-1 permanently. Neither HotSpot nor V8 has a "cannot
compile" class — they compile everything. For WasmEdge this is a current-MVP
boundary that excludes SIMD-heavy kernels indefinitely.

### 4.9 No profile handoff across tier boundary

HotSpot's C1-tier-3 **exists** purely to collect MDO for C2 consumption. V8's
feedback vector is the persistent spine across all tier transitions. WasmEdge
tier-1 observes an execution for the threshold count and discards every
detail except the count that triggered tier-up.

### 4.10 Global OSR threshold

`WASMEDGE_OSR_THRESHOLD` is a single integer shared across every loop. Short
tight loops and fat complex loops use the same threshold. HotSpot scales
thresholds by method size. A fat loop's tier-2 compile amortizes much
faster than a tight loop's; they shouldn't share a threshold.

### 4.11 Tier-2 cannot recover from a bad decision

Related to 4.2. Once a function is in tier-2, it's there forever. If tier-2
is slower on some path (e.g. the `mini-module-narrowing` loss on gcc-loops
0.71×), there is no "de-tier" to fall back to. HotSpot not-entrant marking
handles this naturally.

---

## 5. Ideas to adopt — ranked by win / cost

### Tier 1 — cheap wins that directly target measured losses

#### 5.1 Branch-guarded speculative direct call for `call_indirect`

```llvm
%actual = load ptr, %table_slot
%hit    = icmp eq ptr %actual, @guessed_target
br i1 %hit, label %fast, label %slow
fast:   call @guessed_target(...)        ; LLVM inlines this freely
slow:   call @entry_thunk_proxy(...)     ; current path
```

Matches V8's monomorphic IC shape. **No deopt needed** — the miss path is
functionally correct, just slower. LLVM's O2 sees `@guessed_target` as a
known callee and inlines it in the `%fast` arm. Directly targets ed25519's
130-indirect-calls-per-iter by collapsing the common case to a typed direct
call.

**Cost:** a few hundred LOC in `tier2_compiler.cpp`; one new LLVM helper.
**Expected win:** measurable reduction of the ed25519 0.72× gap and any
kernel whose hot path is `call_indirect`-dominated.

#### 5.2 One-slot `call_indirect` target histogram in tier-1

Emit a single `IR_STORE_A indirect_target_history[site] = resolved_target`
at every `call_indirect` in tier-1. One store; negligible cost. Feeds 5.1.
Conceptually a V8 monomorphic IC with one slot.

#### 5.3 Profile-driven batch scoring

Replace the `(depth<=2, size<=12, static_freq>=2 || counter!=0)` BFS with a
priority-queue expansion:

1. Seed with `(anchor, priority = ∞)`.
2. Pop highest-priority call edge `caller → callee`.
3. Compute `score = observed_callsite_count / estimate(callee_ir_size)`.
4. If `score >= threshold` and global budget allows, add `callee` to the
   batch, expand its callees.
5. Stop when budget exhausted.

This is HotSpot's `InlineTree` collapsed into ~50 LOC. Gets the right 12
functions for ed25519, not just the first 12 the BFS happens to find.

#### 5.4 Immutable-table static analysis

Scan Wasm module for writes to each table. If a table is never mutated
post-instantiation (the common case for Rust/C++/Go compiled Wasm, whose
function tables are initialized from element segments and never touched
again), mark it *immutable*. Every `call_indirect` into an immutable table
with a compile-time-known index is a direct call.

No profile collection required. This is the Wasm analog of HotSpot's CHA —
and unlike CHA it doesn't need a dependency/invalidation mechanism, because
Wasm tables are explicitly mutable vs not via static analysis.

**Expected win:** compound with 5.1/5.3 on ed25519-class kernels; may reveal
entire loops as inlinable that currently see any indirect call.

### Tier 2 — medium-effort, medium-win

#### 5.5 Dynamic OSR threshold by loop size

Scale `OSR_THRESHOLD` per loop by the body's tier-1 IR instruction count:
fat loops trip earlier (compile amortizes fast), tight loops trip later
(compile must amortize against many iters).

#### 5.6 Multi-worker compile pool with priority

Separate `HotOsrQueue`, `HotBatchQueue`, `WarmBacklogQueue`; N worker
threads; pull from hottest non-empty. Addresses the `rhr` / `rust-json`
worker-backlog tail. Straightforward port of HotSpot's `CICompilerCount`
pattern.

#### 5.7 Code cache eviction with not-entrant protocol

1. Mark a tier-2 function not-entrant: `FuncTable[idx]` swapped *back* to
   tier-1 pointer (so new callers go to tier-1).
2. Keep the ORC LLJIT alive until quiescence — no frame executing the
   tier-2 body.
3. Drop the LLJIT; tier-1 carries the function from here.

Quiescence check needs a safe-point mechanism. HotSpot's sweeper walks
thread stacks; a simpler approach is a per-LLJIT refcount incremented at
thunk entry and decremented at exit (at the cost of a 2-instruction
bracket on every call).

### Tier 3 — high-effort, high-win

#### 5.8 Full deoptimization infrastructure

Biggest architectural addition. Build:

- A **deopt map** per tier-2 safepoint recording Wasm-visible state (locals,
  operand stack, pc) as `{vreg | constant | stack-slot}` mappings.
- An LLVM intrinsic `@wasmedge_deopt` the backend emits as a trap returning
  via a per-compile deopt stub.
- The stub reconstructs a tier-1 frame on top of the current native frame
  and resumes in `ir_jit_engine`.
- Deopt sites can be driven by guard checks (speculative inlining, aligned
  memory, etc.) or by "this tier-2 compile was invalidated" markers (e.g.
  table mutated after immutability assumption).

This unlocks every speculation V8 does. Wasm's explicit locals make the
deopt map simpler than JS. Still a multi-month project and only worth it if
5.1–5.4 don't close enough of the gap.

#### 5.9 MDO-equivalent profile record

Once 5.2 exists, generalize: per-function profile struct with
`{call_counts, backedge_counts, indirect_target_history[N], branch_taken[M]}`.
Hand to tier-2 at batch-selection time. Drives better BFS (5.3), better
speculation (paired with 5.8), better OSR prioritization (5.5).

### Not worth porting

- **Sparkplug-equivalent baseline below tier-1.** Would help only cold-start
  latency. WasmEdge's server target doesn't emphasize it; tier-1 is already
  fast enough for warm workloads. The design cost (another code path,
  another ABI) isn't justified.
- **Sea-of-Nodes IR change in tier-1.** Tier-2 already uses LLVM, which is
  Sea-of-Nodes-equivalent internally. Switching dstogov/ir would be a
  rewrite with no obvious payoff.
- **Generic hidden-class / shape machinery.** Wasm is statically typed —
  there's nothing to shape-analyze. The JS-specific complexity doesn't
  translate.

---

## 6. Recommendation — what to build next

If the goal is closing the measured honest losses vs LLVM JIT (ed25519 0.72×,
blind-sig 0.81×, gcc-loops 0.71×, fib2 0.78×, sieve 0.81×, rust-* 0.86–0.92×),
the cheapest path is this order:

1. **5.2** one-slot `call_indirect` target histogram in tier-1 (~50 LOC)
2. **5.1** branch-guarded speculative direct call in tier-2 (~200 LOC)
3. **5.4** immutable-table static analysis (~100 LOC, purely static)
4. **5.3** profile-driven batch scoring replacing BFS (~200 LOC)

Expected bundle outcome: ed25519, blind-sig, and every other
`call_indirect`-dominated kernel closes substantially against LLVM JIT;
gcc-loops and fib2 likely do not (they're whole-module-vectorizer losses,
not indirection losses).

If **after** that bundle the remaining losses still justify it, 5.8
(deoptimization) becomes the next lever — at which point the WasmEdge IR
JIT would be architecturally equivalent to a Maglev-class engine over Wasm
instead of JS.

---

## 7. TL;DR

The WasmEdge IR JIT is a **two-tier optimizing-to-more-optimizing** pipeline:
both tiers are real optimizers and the lower tier exists mostly as fast
working code. HotSpot and V8 are **multi-tier profile-collecting-to-
optimizing** pipelines: lower tiers exist primarily to produce profile data
(MDO, feedback vectors) the upper tier specialises against, and
deoptimization gives upper tiers permission to speculate aggressively.

The current 2026-04-19 benchmark shape — 18/32 kernels at-or-beating LLVM JIT,
losses concentrated in multi-call / whole-module-inlined kernels — is exactly
what the missing profile/speculation axis predicts. The cheapest win is to
collect one piece of profile data (`call_indirect` target per site) and emit
one guarded-direct-call pattern in tier-2. That alone closes the class of
loss most visible in the benchmark, without adopting the full
deopt/feedback-vector architecture HotSpot and V8 depend on.

---

## Sources

- V8: [Maglev — V8's Fastest Optimizing JIT](https://v8.dev/blog/maglev)
- V8: [Sparkplug — a non-optimizing JavaScript compiler](https://v8.dev/blog/sparkplug)
- HotSpot: [How Tiered Compilation works in OpenJDK (Microsoft)](https://devblogs.microsoft.com/java/how-tiered-compilation-works-in-openjdk/)
- HotSpot: [Introduction to HotSpot JVM C2 JIT Compiler (eme64 blog)](https://eme64.github.io/blog/2024/12/24/Intro-to-C2-Part01.html)
- HotSpot: [Tiered Compilation in JVM (Baeldung)](https://www.baeldung.com/jvm-tiered-compilation)
- HotSpot: [Compilation in the HotSpot VM (ETH slides)](https://ethz.ch/content/dam/ethz/special-interest/infk/inst-cs/lst-dam/documents/Education/Classes/Fall2015/210_Compiler_Design/Slides/hotspot.pdf)
- V8: [Profile-Guided Tiering in V8 (Intel)](https://community.intel.com/t5/Blogs/Tech-Innovation/Client/Profile-Guided-Tiering-in-the-V8-JavaScript-Engine/post/1679340)

---

## 8. Improvements under the "simple but efficient" constraint

The project aim is simple + efficient. This section filters Section 5's menu
down to only the items that close measured gaps without adding subsystems,
new ABIs, or new data structures the rest of the codebase has to know about.

### Tier A — do these (cheap, direct impact on measured losses)

**1. One-slot `call_indirect` target histogram in tier-1 (~50 LOC).**
One `IR_STORE_A last_target[site] = resolved_ptr` per `call_indirect`. No new
data structure beyond a per-module array. Zero runtime cost on the hot path.

**2. Guarded speculative direct call in tier-2 (~200 LOC).**

```
if (actual == guessed) direct_call(guessed); else proxy_thunk(idx);
```

No deopt needed — the miss path is just the current slow path. LLVM inlines
`@guessed` in the fast arm for free. Directly attacks ed25519's 130
indirect-calls-per-iter.

**3. Immutable-table static analysis (~100 LOC, pure static).**
Scan the module at instantiation: if a table has no `table.set` / `table.fill`
/ `table.copy` / `table.init` *after* its elem segments, mark it immutable.
Tier-2 can then treat every entry as a constant function pointer — compounds
with (2) because LLVM sees the table load as loading a constant.

**4. Profile-driven batch priority (~150 LOC).**
Replace `bfsDownBatchLocked`'s depth-2/size-12 BFS with a priority queue keyed
on `callsite_count / callee_ir_size`. Same budget (12), smarter selection.
Fixes the ed25519 "first 12 BFS happens to find" failure mode without raising
complexity.

**Bundle total: ~500 LOC, no new subsystems.** Expected to close ed25519 /
blind-sig / ctype-class losses substantially. The shape of the code (optional
histogram, guarded branch, priority queue) is local to files already owned.

### Tier B — small tweaks with diminishing returns

- **`llvm.expect` on tier-1 branch-taken counts.** If already collecting, pass
  them to the LLVM frontend as branch-weight metadata. ~30 LOC, modest win on
  branchy kernels.
- **Bump `MaxBatchSize_` to 16–20.** Single constant change. Cheap test; if
  compile latency is fine, take the free scope.
- **Dynamic `OSR_THRESHOLD` by body size.** One multiplication at instrument
  time.

### Tier C — don't do these (high cost, violates "simple")

- **Deoptimization.** Gives V8/HotSpot their speculation ceiling but needs
  deopt maps, safepoints, frame reconstruction — months of work and permanent
  complexity. Skip.
- **MDO-equivalent profile record / feedback vectors.** Same — too much
  structure.
- **Sparkplug-equivalent third tier.** Not the latency regime.

### The honest losses this design can't cheaply fix

`gcc-loops 0.71×`, `fib2 0.78×`, `sieve 0.81×`, `rust-compression 0.77×` are
**whole-module vectorizer / cross-function SCCP** deltas. No amount of
indirect-call cleverness closes them — LLVM JIT sees the whole program and the
mini-module sees 12 functions. Accept these; they're the honest cost of the
mini-module boundary chosen for a reason (latency). The benchmark already
documents this tradeoff.

**Bottom line:** build Tier A. That's the full set of simple wins with
measured-loss alignment. Tier B is noise; Tier C breaks the "simple"
constraint and the remaining gap after Tier A likely doesn't justify it.
