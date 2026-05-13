# Regular tier-2 vs OSR: what each path actually earns

**Date:** 2026-04-23 (superseding the initial 2026-04-23 version with EXP-run corrections)
**Companion docs:** `tier2_v2_doc.md`, `osr_doc.md`
**Source evidence:** sightglass-strong, Release build with P1 stub pruning, 3-run median + traced single runs (`SPDLOG_LEVEL=info`, `WASMEDGE_TIER2_DUMP_IR`), plus a counterfactual sweep (`EXP`) where the regular-tier-up path was disabled while OSR and `fN_entry_thunk` were kept.

## Purpose

The tier-2 pipeline has three distinct acceleration mechanisms that feed
the same mini-module compiler:

1. **Regular tier-2 `FuncTable[F]` swap.** Triggered by
   `CallCounters[F] >= TIER2_THRESHOLD` (default 10). The swap changes
   the destination of **every future tier-1 `call F` instruction** in
   the module — regardless of which caller.
2. **OSR `OsrEntryTable[F*16+L]` swap.** Triggered by
   `BackEdgeCounters[F*16+L] >= OSR_THRESHOLD` (default 5000). The swap
   migrates the **currently running frame of F** into LLVM code
   mid-loop at its next back-edge poll.
3. **`fN_entry_thunk`** (regular-tier-2 path, instantiation-time).
   One LLVM-ABI wrapper per IR-JIT function, built before execution
   starts. Makes `call_indirect` from LLVM-compiled code into
   IR-JIT-backed targets a direct typed call.

Which one is doing the work on any given kernel cannot be answered by
inspecting the logs alone (both fire on most kernels). It has to be
answered by a counterfactual: disable one path, measure. This doc
reports that measurement and the resulting mental model.

An earlier draft of this doc divided kernels into "regular tier-2
owns" vs "OSR owns" based on which path fired first in the log. That
test was wrong — "fired first" is not the same as "is load-bearing."
The EXP sweep disambiguates and the categorization below has been
corrected accordingly.

## Tier-1's direct-call lowering

The reason the FuncTable swap is potent is visible in
`lib/vm/ir_builder.cpp::visitCall`, lines 3263–3267:

```cpp
} else {  // direct call (not import, not call_indirect)
  ir_ref ValidFT  = ensureValidRef(FuncTablePtr, IR_ADDR);
  ir_ref FuncPtr  = ir_LOAD_A(ir_ADD_A(
      ValidFT,
      ir_CONST_ADDR(ResolvedFuncIdx * sizeof(void *))));
  // ... marshal args, call through FuncPtr ...
```

Every direct `call N` site emits a load from `FuncTable + N*8` **at
runtime**, on every call. There is no caching, no one-shot resolution.
So when regular tier-2 performs
`FuncTable[N] = f<N>_fwd_thunk`, **every subsequent tier-1 call site
that names N** — anywhere in the module, including recursive
self-calls, including calls from other tier-1 helpers, including
`t1_thunk` dispatches made by OSR bodies — immediately starts landing
on the LLVM-compiled fwd_thunk.

This one-line lever is what distinguishes the FuncTable swap from
OSR. OSR compiles an LLVM-ABI copy of a specific function with its
BFS'd callees inlined and publishes it to one slot that one frame
polls. FuncTable swap publishes a pointer to a different slot that
every tier-1 `call N` instruction in the module loads.

## The EXP experiment

To separate "regular tier-2 fired" from "regular tier-2 was
load-bearing", a gate was added in `Tier2Manager::enqueue` that
early-returns the non-OSR branch when `WASMEDGE_TIER2_DISABLE_REGULAR=1`.
This preserves:

- `fN_entry_thunk` construction at instantiation time (regular-tier-2
  path's instantiation hook),
- OSR trigger + enqueue + BFS batching,
- tier-1 codegen (the call-count prologue and `CallCounters` array
  remain in place — deleting them would need tier-1 changes; the
  experiment measures what the runtime tier-up contributes on top of
  OSR + entry thunks, not the cost of the prologue).

3-run median WT, sightglass-strong, Release, P1 stub pruning enabled,
32 kernels aggregated (`noop` excluded):

| Metric | P1 | EXP | Δ |
|---|---:|---:|---:|
| Geomean tier-1 / tier-2 WT | 1.136× | **1.078×** | −5.1 pp |
| Geomean LLVM-JIT / tier-2 WT | 0.925× | **0.878×** | −4.7 pp |
| Geomean Δ tier-2 WT | — | **1.054×** | EXP 5.4% slower |

33/33 kernels pass golden output. The regression is perf-only.

## Per-kernel results from EXP

### Regressions where regular tier-2 was load-bearing

| Kernel | P1 WT | EXP WT | Δ | P1 t1/t2 → EXP t1/t2 |
|---|---:|---:|---:|---|
| **shootout-fib2** | 6,546k | 13,210k | **2.02×** | 1.21× → **0.60×** |
| **shootout-ackermann** | 6,167k | 11,350k | **1.84×** | 1.67× → 0.91× |
| rust-protobuf | 2,607k | 3,033k | 1.16× | 1.05× → 0.91× |
| rust-json | 3,318k | 3,587k | 1.08× | 0.98× → 0.91× |
| bz2 | 7,237k | 7,660k | 1.06× | 1.09× → 1.03× |
| hashset | 5,998k | 6,265k | 1.04× | 0.95× → 0.91× |
| shootout-memmove | 2,691k | 2,796k | 1.04× | 1.01× → 0.97× |
| gcc-loops | 7,907k | 8,142k | 1.03× | 0.98× → 0.95× |

fib2 and ackermann collapse to **below tier-1** when regular tier-up
is gone, demonstrating that regular-tier-2 was contributing more than
OSR on these kernels. rust-protobuf drops from slightly-above-tier-1
to below-tier-1 — tier-2 goes from helpful to actively harmful without
the FuncTable-swap path.

### Kernels that improved without regular tier-up

| Kernel | P1 WT | EXP WT | Δ | Why |
|---|---:|---:|---:|---|
| blind-sig | 4,782k | 4,324k | **0.90×** | Regular-tier-2 batches were queued behind OSR compiles and stealing worker CPU. Without them, OSR lands sooner and the migrated frame runs LLVM code for more of the benchmark. |

### Neutral (±1%, effectively noise)

24 of 32 kernels, including `shootout-random`, `shootout-ctype`,
`shootout-ed25519`, `quicksort`, `shootout-matrix`, `shootout-keccak`,
`shootout-base64`, `shootout-minicsv`, `shootout-ratelimit`,
`shootout-switch`, `rust-compression`, `rust-html-rewriter`, etc.

These are the kernels where either OSR's BFS-inlined batch already
covers the hot path (random, ctype, ed25519), or the win comes from
the `fN_entry_thunk` path which the experiment preserved (minicsv,
ratelimit), or tier-2 doesn't meaningfully help regardless of which
mechanism runs (sieve, switch, rhr, cmark — structural limits).

## How regular-tier-2 produces its wins

The EXP regressions group into three classes by mechanism.

### Class A — Direct recursion (fib2 2.02×, ackermann 1.84×)

Traced fib2 (single run):

```
+14 ms  tier2: batch compile: root=9 hot=10 size=3 [9,10,19]
+35 ms  tier2: upgraded func 10 → tier-2              ← recursive fib
+35 ms  tier2: upgraded func 19 → tier-2              ← helper
... kernel runs 6,541 ms ...
```

`f10` is the recursive `fib` function; its body contains `call 10`
(two self-calls). Every tier-1 fib frame loads `FuncTable[10]` on each
recursive call. Once the swap lands at +35 ms, the **entire recursion
tree transitions to LLVM** — a tier-1 fib frame's next `call 10`
dispatches to the tier-2 fwd_thunk which runs the LLVM body, which
does its own `call 10`, which again lands on tier-2.

OSR cannot reach this shape. Its trigger is back-edges in wasm `loop`
openers; recursive self-calls are not `loop` openers. OSR fires
`size=1 [10]` on fib and migrates the topmost fib frame, but that
frame's recursive calls still go through `FuncTable[10]` — without
regular tier-2's swap, they land on tier-1 and the rest of the tree
runs tier-1. Result: EXP fib2 runs 13.2M µs instead of 6.5M.

Same mechanism on ackermann: two regular batches cover the
mutually-recursive ackermann-like functions ([58,75,57] and
[16,14,47,15]); without them the recursion tree stays on tier-1.

**This class of kernel structurally depends on regular tier-2.** No
extension to OSR's batching can reproduce the FuncTable-swap's reach
across recursive call sites.

### Class B — Wide helper graphs (rust-protobuf 1.16×, bz2 1.06×, memmove 1.04×, hashset 1.04×)

These kernels have many small helpers called from many sites in the
module. Regular tier-2 promotes the helpers as their call counts trip
10. Each swap cheapens every `call <helper>` site across the module
simultaneously.

Two mechanisms contribute on top of OSR:

1. **Direct calls from any tier-1 body** to the promoted helper. On
   bz2, helpers `19, 20, 71, 81, 103` are called from several tier-1
   functions that aren't themselves OSR targets. Without the FuncTable
   swap, all those calls stay on tier-1 helpers.
2. **`t1_thunk` dispatches from OSR bodies' out-of-batch calls.** OSR
   batches cap at 12. The OSR body's `call` instructions for funcs not
   in its batch lower to `t1_thunk → FuncTable[helper]`. If regular
   tier-2 already swapped that slot, LLVM body calls LLVM helper. If
   not, LLVM body calls tier-1 helper.

rust-protobuf shows the effect most strongly (+16%) because it has the
widest call graph: 55 regular upgrades visible in the trace, and OSR
batches averaging 4 members against ~15 direct callees per OSR body.
The "out-of-batch" set is large; regular tier-2's FuncTable swaps are
the backing store.

**This class would need either much larger OSR batch caps (with the
compile-cost blowup that follows) or per-call inlining budgets that
don't exist in the current design to replicate the win without regular
tier-2.**

### Class C — `call_indirect`-heavy (minicsv, ratelimit, switch)

Confirmed the earlier analysis. The win comes from `fN_entry_thunk`
adapters built at instantiation time. The experiment preserved that
hook (it's separable from the runtime tier-up machinery, though it
lives on the regular-tier-2 instantiation path), so WT is unchanged
±0.2%.

**Regular tier-2 "owns" this class only in the sense that the
instantiation hook is currently hosted on its path.** The hook itself
has nothing to do with runtime call-count promotion and could
theoretically move elsewhere.

### Class D — Pre-OSR window on iterated kernels

Between `CallCounters[helper] >= 10` (fires on iteration 10) and
`BackEdgeCounters[outer*16+L] >= 5000` (fires on iteration ~5000), the
tier-1 outer body's calls to the helper land on the already-swapped
`FuncTable[helper]`. This is a small but non-zero contribution on
long-running benchmarks. EXP didn't isolate this from the larger
effects, but the near-zero Δ on `shootout-random`, `shootout-ctype`,
etc. suggests it is a percent-level contribution at most.

## The mechanisms, re-ordered

| Mechanism | Reach | Per-site speedup | What it can't do |
|---|---|---|---|
| **FuncTable[F] swap** (regular tier-2) | **Every** tier-1 `call F` site + every `t1_thunk` dispatch + every `call_indirect` resolving to F | Cheap LLVM call (fwd_thunk + ABI marshal, no inlining) | Inline a helper into its caller |
| **OsrEntryTable swap** (OSR) | One frame on one loop | Deep inlining of BFS-batched callees | Accelerate call sites outside the OSR'd frame |
| **fN_entry_thunk** (instantiation) | Every LLVM `call_indirect` into IR-JIT target | Direct typed LLVM call instead of executor re-entry | Anything other than indirect-call ABI bridging |

No pair of these replicates the third. FuncTable swap's wide reach is
what makes direct recursion (Class A) and wide helper graphs (Class
B) fast. OSR's deep-inlining of a specific batch is what makes
one-shot outer frames fast (blind-sig, ctype's predicates inlined into
f9's OSR body). Entry thunks are what make `call_indirect` fast.

## Decision matrix (corrected)

| Shape | Who owns the win | Evidence |
|---|---|---|
| **Direct recursion, no wasm `loop`** (fib2, ackermann) | Regular tier-2 FuncTable swap | EXP: fib2 2.02×, ackermann 1.84× regression |
| **Wide helper graph called from many tier-1 bodies** (rust-protobuf, bz2, hashset, memmove) | Regular tier-2 FuncTable swap (primary) + OSR on outer loop (secondary) | EXP: rust-protobuf 1.16×, bz2 1.06%, memmove 1.04× regression |
| **One-shot outer frame + BFS-inlineable helpers** (blind-sig, ctype, ed25519, random, matrix, keccak, base64, quicksort) | OSR with helpers batched | EXP: all neutral or slightly improved; blind-sig 0.90× win |
| **`call_indirect`-heavy dispatch** (minicsv, ratelimit, switch, some of protobuf) | `fN_entry_thunk` at instantiation | EXP: all neutral |
| **Tier-2 doesn't meaningfully help** (rust-json, rhr, gcc-loops, sieve) | Structural ceiling — mini-module scope vs whole-module LLVM | Both P1 and EXP around 0.66–0.85× of LLVM JIT |

## Why keep all three mechanisms

The EXP sweep is the measurement that settles "keep both or collapse
one":

- **Delete OSR:** lose ~50% of blind-sig's 2.04× win (one-shot signing
  loop), plus inlining-driven wins on ctype/ed25519/random. Not
  measured here directly, but `osr_doc.md` §1 and the initial
  2026-04-15 tier-2-only benchmark give the ceiling.
- **Delete regular tier-up (EXP):** lose fib2 (2.02× regression),
  ackermann (1.84×), rust-protobuf (1.16×), and ~5 pp of geomean vs
  tier-1.
- **Delete `fN_entry_thunk`:** minicsv / ratelimit / switch lose their
  `call_indirect` acceleration (prior benchmark data before P1g
  showed ratelimit at 0.33× of tier-1).

Each mechanism is load-bearing on a measurable class of kernels, and
the classes do not overlap meaningfully. The design already
consolidates what can be consolidated: shared `Tier2Manager::enqueue`,
shared `walkUpRootLocked` + `bfsDownBatchLocked`, shared
`Tier2Compiler::compileRequest`. What remains distinct — the trigger
counter, the publication slot, and the instantiation hook — cannot be
fused because they operate on different runtime state (future calls,
in-flight frames, indirect dispatch targets).

## References

- `tier2_v2_doc.md` — mini-module synthesis, fwd_thunk / t1_thunk /
  entry_thunk, batch composition rules, P1g entry-thunk mechanism.
- `osr_doc.md` — OSR trigger diamond, `synthesizeOsrModule`, BFS
  batching with helpers, §12.1 (batch composition was necessary for
  `ctype` to reach LLVM-JIT parity).
- `notes/benchmarking/total_improvement.md` — 89e83770 → 616527b5 → P1
  progression.
- `lib/vm/ir_builder.cpp::visitCall` (lines 3263–3267) — the direct-call
  FuncTable load that makes the regular-tier-2 swap reach every call
  site.
