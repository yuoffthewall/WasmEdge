# blind-sig: real root cause of the tier-2 / LLVM-JIT gap

**Date:** 2026-04-19
**Branch:** `osr`
**Method:** OSR per-function bisection using existing `WASMEDGE_OSR_MIN_FUNC` /
`WASMEDGE_OSR_MAX_FUNC` env vars. No code changes.

---

## TL;DR — corrects the previous (wrong) writeup

Previous draft claimed mac3's 43-outermost-loop count and the
`OSR_MAX_LOOPS_PER_FUNC = 16` cap as the root cause. **Wrong.**
Implementing variable-size storage to remove that cap delivered only
~3% on blind-sig and was reverted. The chase started from a structural
hypothesis that was never bisected against the actual cost
distribution.

The real answer, from per-function bisection (numbers below):

> blind-sig is a **one-shot kernel**. `benchmark::main` (f80) has zero
> loops — one signing operation per kernel invocation, no warm-up
> iterations. The dominant hot function is **f229
> (`num_bigint_dig::biguint::monty::montgomery`)**, the Montgomery
> reduction step inside the modular exponentiation. **OSR-only-f229
> closes ~95% of the entire tier-2+OSR benefit.** Every other
> function's OSR is either neutral or net-negative (the per-iteration
> back-edge poll diamond costs more than the OSR'd code saves on cold
> functions).
>
> The remaining ~1.5 s gap to LLVM JIT (5.20 s vs 3.71 s) is
> structural and **independent of OSR install latency** — varying
> `WASMEDGE_OSR_THRESHOLD` from 1 to 50,000 changes WT by less than
> 3 %. The gap is the cost of mini-module narrowing (LLVM JIT inlines
> across the full module, mini-module batches do not) plus the
> per-call indirect dispatch through `FuncTable[idx]` between
> separately-compiled tier-2 fwd_thunks.

---

## Reproduced setup

`sightglass-strong/blind-sig/benchmark.wasm`. Single-kernel runs, isolated, 3-run
median, `WASMEDGE_QUIET=1`. WT is the time between the wasm's
`bench.start` and `bench.end` calls — apples-to-apples across arms.

Kernel structure (from `wasm-objdump -d`):

- `_start` (f15) → `__main_void` (f81) → `benchmark::main` (f80)
- `benchmark::main` body: ~11 sequential `block { … }` scopes, **zero
  `loop`**. Single `call sightglass_api::start` near the top, single
  `call sightglass_api::end` near the end. One signing operation per
  kernel invocation.
- The signing op proceeds through DER decode of the secret key,
  message blinding (`raw_encryption_primitive` f102 — RSA pubkey op),
  Montgomery exponentiation (`monty_modpow` f230) which calls
  `montgomery` (f229) inside its outer `square+multiply` loop, and
  RSA-PKCS#1-v1.5 finalization.

---

## Bisection: which OSR'd function actually delivers the speedup?

Each row enables OSR for exactly the listed function(s) and tier-2
function-entry compilation for everything (`THRESHOLD=10`,
`OSR_THRESHOLD=5000`). 3-run median.

| Configuration | WT (µs) | Δ vs T2-only | Notes |
|---|---:|---:|---|
| Tier-1 only (no T2, no OSR) | 9,484k | — | reference |
| **T2 only** (no OSR) | **7,744k** | baseline | function-entry tier-2 alone |
| OSR-disabled-for-f229 (range 14-205) | 8,187k | **+443k (worse!)** | every other function gets OSR; without f229 the cost dominates |
| OSR-only-f206 (mac3) | 7,933k | **+189k (worse!)** | OSR poll cost > benefit |
| OSR-only-f231 (div_rem) | 7,902k | +158k | net-negative |
| OSR-only-f195 (BigInt::Mul) | 7,821k | +77k | net-negative |
| OSR-only-f230 (monty_modpow) | 7,698k | −46k | ~zero |
| OSR-only-f102 (raw_enc_primitive) | 7,736k | −8k | ~zero |
| **OSR-only-f229 (montgomery)** | **5,196k** | **−2,548k (−33%)** | **single function ≈ entire OSR benefit** |
| OSR-only-f229+f230 | 5,994k | −1,750k | adding f230 actually loses ground |
| **T2+OSR all functions** | **5,057k** | −2,687k | only ~3% better than f229-only |
| LLVM JIT (whole-module) | 3,706k | — | reference |

Two things this table forces:

1. **One function carries the entire OSR speedup.** Disable OSR for
   f229 alone (the "14-205 range" row) and tier-2+OSR is *worse* than
   tier-2 alone. The other 40 OSR notifies that fire on a typical run
   contribute essentially nothing.
2. **OSR for cold functions is a perf tax.** The back-edge poll
   diamond emitted in tier-1 (load `OsrEntryTable[idx]`, compare,
   branch — see `lib/vm/ir_builder.cpp:2283-2298`) runs on every
   iteration of every instrumented loop, and the OSR'd code never
   delivers enough win on cold functions to amortize it.

## Bisection: does install latency matter?

Same as above but vary `OSR_THRESHOLD` to change when the OSR notify
fires (and therefore when the worker can start compiling the
continuation). 3-run median, OSR restricted to f229.

| `OSR_THRESHOLD` | WT (µs) | Notes |
|---:|---:|---|
| 1 | 5,188k | trips on first iteration; install lands almost immediately |
| 100 | 5,129k | (best) |
| 1,000 | 5,231k | |
| 5,000 | 5,257k | (default for sightglass-strong sweeps) |
| 50,000 | 5,279k | install lands much later in the kernel |

**Spread is ~3 % across a 50,000× threshold range.** Whether the OSR
continuation lands at 100 µs into the kernel or at 1 s into it, the
final WT is essentially the same. So:

- The latency between the back-edge counter saturating and the OSR
  entry getting installed is **not** the bottleneck.
- "Multiple worker threads" / "earlier OSR install" wouldn't move the
  needle on blind-sig.

## What the gap actually is (5.20 s vs LLVM-JIT 3.71 s)

With f229 OSR'd at any reasonable threshold, ~70 % of the kernel runs
through f229's LLVM-compiled mini-module body. The remaining ~1.5 s
gap to LLVM JIT comes from two structural sources, both **independent
of OSR coverage**:

### 1. Mini-module narrowing reduces LLVM's optimization scope

The OSR mini-module for f229 contains f229's body plus its direct
callees within depth-2 BFS (`tier2_compiler.cpp:1316-1320`). LLVM's
inliner sees **only** that batch when deciding what to inline. Whole-
module LLVM JIT sees the entire 565-function module and inlines
across boundaries that the mini-module batch cannot.

Concretely: f229 has 9 native `mul nuw i64` instructions in *both* its
OSR'd and LLVM-JIT'd versions, so the inner-loop multiply codegen is
equal. The difference is what surrounds those muls — LLVM JIT inlines
helper allocations, sinks loads across calls, hoists invariants out
of nested conditionals — opportunities the mini-module cost model
cannot price because the surrounding context isn't in scope.

### 2. Cross-tier-2 calls go through `FuncTable[idx]` indirect dispatch

When OSR'd f229 calls another tier-2-compiled function (e.g.
`SmallVec::extend` at f220-f223), the call lowering goes through
`FuncTable[targetIdx]` — load + indirect call. Whole-module LLVM JIT
emits a direct `call @f<idx>` (which the linker resolves to a fixed
relative jump). Per-call delta is ~5-10 cycles; over the inner loop's
millions of helper calls, this adds up.

Neither of these can be closed by changing OSR coverage policy or
worker scheduling. Closing them requires either:

- **Whole-module mini-batches** — bring more callees into the same
  ORC module so calls are direct; but this gives up the per-batch
  latency advantage that motivated the tier-2 design.
- **Inline the mini-module into a shared module** at install time —
  same trade-off.

These are honest limits of the function-granularity tier-2 design.

## What this corrects from the previous draft

| Old hypothesis | Status |
|---|---|
| "mac3 has 43 outermost loops; the hot one is loop #42; cap=16 blocks it" | **Wrong.** Bisection shows mac3 OSR is *net-negative* (7,933k vs T2-only 7,744k). mac3 isn't on the hot path; the previous count of `call __multi3` sites (3 in mac3) ≠ how often those calls execute. The real call graph: `monty_modpow` → `montgomery` (the 9-mul inner loop). mac3 is on a colder path (called from `BigUint::modpow`, `div_rem`, `BigInt::Mul`). |
| "OSR_MAX_LOOPS_PER_FUNC = 16 is a hard ceiling that needs removing" | **Wrong as a perf fix.** Removing it (variable-size offset table, ~150 lines of code, tested 33/33 sightglass-strong pass) gave only 3% on blind-sig. The cap *was* truncating mac3's loop 42, but mac3 wasn't hot. Reverted. |
| "Worker-thread parallelism would close more of the gap" | **Wrong on blind-sig.** Threshold sweep proves install latency doesn't matter here. Multi-worker would help OSR-bound workloads where many functions actually need OSR (e.g. ed25519, where OSR-only-* would presumably split differently). |

The previous draft was guided by code-structure hypotheses
("OSR_MAX_LOOPS_PER_FUNC is suspicious" + "mac3 has many loops") and
extrapolated without bisecting. The bisection results force a much
narrower and accurate story.

## What "improving" means for blind-sig

Useful directions, ordered by how directly they target the bisected
bottleneck:

1. **Skip OSR instrumentation for functions whose loops are unlikely
   to be hot.** The poll diamond costs every iteration, regardless of
   whether it ever fires. A static heuristic ("function body has < N
   instructions and no `call_indirect`") would catch the worst
   offenders. Or: keep counters only for outermost loops *with no
   nested loop body containing `call`* (suggesting they're tight
   helper loops where OSR is meaningful). Either reduces the
   net-negative tax we measured (8,187k − 7,744k = 443k slower when
   OSR fires for the wrong functions).
2. **Pull more callees into the OSR batch for f229.** Currently the
   batch is `{f229, helpers}` at depth 2. Bumping depth/size for
   *this specific compile* (heuristically: when the OSR root is the
   single hottest function) might let LLVM inline across a wider
   slice. Worth a knob sweep.
3. **Lift one chosen function into a "share with future batches"
   slot.** If f229 is hot enough to OSR, also use it as the seed of a
   regular tier-2 batch and put it in a shared module. Subsequent
   batches that include f229 as a callee then call it directly. This
   would close part of the cross-tier-call gap.

Things that would *not* help on blind-sig (per the data above):
- Multi-worker / faster OSR install
- Removing `OSR_MAX_LOOPS_PER_FUNC = 16`
- More-aggressive function-entry tier-2 batching

## Takeaway for methodology

The variable-storage chase happened because I jumped from "mac3 has 43
loops" → "the cap must be the cause". The bisection that would have
disproved this took 5 minutes once I actually ran it
(`OSR_MIN/MAX_FUNC` was already in the code). For future tier-2
investigations: **bisect OSR scope first, then form hypotheses about
which functions matter.** Code-structure inspection alone is not
sufficient evidence.
