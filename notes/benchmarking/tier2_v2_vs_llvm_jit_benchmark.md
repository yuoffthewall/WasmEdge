# Tier-2 V2 vs LLVM JIT on `sightglass-strong`

## Purpose

Measure the rewritten tier-2 pipeline ("V2": mini-module synthesis →
`LLVM::Compiler::compile` → ABI bridge thunks → ORC LLJIT) against the
whole-module LLVM JIT path on `sightglass-strong`. The question is
whether tier-2 V2, which promotes only hot functions plus their depth-1
callees, can reach parity with LLVM JIT, which compiles the entire
module up front.

Background on the strengthened suite — why it exists, per-kernel
strengthening, harness plumbing — lives in
`tier2_vs_ir_jit_benchmark_strong.md`. This document only covers the
tier-2 V2 ↔ LLVM JIT comparison and follow-up investigations.

## What this benchmark measures

Tier-2 V2 is the post-rewrite tier-2 pipeline: the dstogov/ir →
`ir_emit_llvm` → LLVM IR text path is gone. Tier-2 now synthesizes a
per-batch mini `AST::Module` and lowers it through the WasmEdge LLVM
frontend (`lib/llvm/compiler.cpp`), then post-processes the resulting
`llvm::Module` with two ABI bridges (`fN_fwd_thunk` for tier-1 → tier-2
via FuncTable swap, `fN_t1_thunk` as an in-place rewrite of non-batch
stub bodies for tier-2 → tier-1 dispatch through
`wasmedge_tier2_get_jit_env`), runs opt at `default<O2>` after the
post-processing, and hands the module to a fresh ORC LLJIT. Scope is
scalar-only (i32/i64/f32/f64, single return), batch size ≤ 12
(hot head + direct callees), promotion triggered by call-count thresholds
of 10 (calls) / 5 (loop back-edges). Full design doc:
`notes/design_docs/tier2_v2_doc.md`.

**LLVM JIT** is `WASMEDGE_SIGHTGLASS_MODE=JIT`, the full AOT lowering
path that runs the same `LLVM::Compiler` at instantiation and executes
the result directly. No tiering, no swap, no background thread.

**Fair opt-level comparison.** Tier-2 V2's post-processing opt pipeline
is `default<O2>`. For an apples-to-apples comparison the LLVM JIT arm
also runs at **`OptimizationLevel::O2`**
(`test/ir/ir_benchmark_test.cpp:1446-1447`). Earlier numbers compared
against LLVM JIT at O3; the O2 switch shifted the WT geomean by ~2% and
flipped exactly one kernel (`gcc-loops`).

Three metrics:
- **Inst.Lat (IL)** — instantiation latency (µs). Time from "got the
  parsed `AST::Module`" to "ready to execute the bench region".
- **WorkTime (WT)** — steady-state work time (µs). Time between the
  `bench.start` and `bench.end` imports. For tier-2 V2 this includes
  any tier-1 execution before the swap plus all post-swap tier-2
  execution.
- **TtV (Time-to-Value)** — total µs from process start through final
  kernel output. Approximately `IL + WT + small fixed overhead`.

Speedup is `LLVM_JIT_median / IR_JIT_tier2_median`; **values > 1 mean
tier-2 V2 is faster**.

## Methodology

- **Suite**: `sightglass-strong` (33 kernels; spidermonkey and tinygo
  excluded per project rules).
- **Runs per mode**: 3 for the main sweep; 10 for `shootout-ackermann`
  and 7 for `shootout-fib2` after variance inspection.
- **Aggregation**: per-kernel per-metric **median** across runs.
  Geometric mean for the aggregate speedup.
- **Build**: Debug cmake build of `wasmedgeIRBenchmarkTests`, LLVM 20,
  Ubuntu 22.04.
- **LLVM JIT opt level**: **O2** (edited in
  `test/ir/ir_benchmark_test.cpp:1446`).
- **Environment**:
  ```shell
  WASMEDGE_SIGHTGLASS_DIR=sightglass-strong
  WASMEDGE_SIGHTGLASS_MODE=IR_JIT   # or JIT for the other arm
  WASMEDGE_IR_JIT_OPT_LEVEL=2
  WASMEDGE_TIER2_ENABLE=1
  WASMEDGE_TIER2_THRESHOLD=10
  WASMEDGE_TIER2_LOOP_THRESHOLD=5
  ```

## Results (median of 3 runs, LLVM JIT at O2)

Times in µs. `spd` columns are `LLVM_O2 / (IR+T2)` — values **> 1** mean
tier-2 V2 wins.

| Kernel | IL IR+T2 | IL LLVM O2 | **IL spd** | WT IR+T2 | WT LLVM O2 | **WT spd** | TtV IR+T2 | TtV LLVM O2 | **TtV spd** |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| blake3-scalar | 38,492 | 708,495 | **18.41×** | 5,695,278 | 5,498,419 | 0.97× | 5,748,964 | 6,211,036 | **1.08×** |
| blind-sig | 218,980 | 3,285,686 | **15.00×** | 11,681,488 | 3,958,780 | 0.34× | 11,983,342 | 7,362,057 | 0.61× |
| bz2 | 97,628 | 1,122,198 | **11.49×** | 7,562,028 | 7,057,505 | 0.93× | 7,681,767 | 8,227,064 | **1.07×** |
| gcc-loops | 513,011 | 4,093,377 | **7.98×** | 7,996,485 | 8,217,998 | **1.03×** | 8,605,100 | 12,296,476 | **1.43×** |
| hashset | 1,052,885 | 2,263,644 | **2.15×** | 11,471,993 | 7,444,495 | 0.65× | 12,599,331 | 9,745,089 | 0.77× |
| noop | 2,642 | 56,961 | **21.56×** | 1 | 1 | 0.91× | 3,739 | 58,038 | **15.52×** |
| pulldown-cmark | 151,175 | 2,050,809 | **13.57×** | 8,598,387 | 7,575,599 | 0.88× | 8,792,284 | 9,672,996 | **1.10×** |
| quicksort | 17,647 | 215,283 | **12.20×** | 7,026,070 | 6,896,126 | 0.98× | 7,047,540 | 7,119,046 | **1.01×** |
| regex | 309,801 | 5,374,022 | **17.35×** | 9,308,655 | 8,839,666 | 0.95× | 9,737,830 | 14,314,832 | **1.47×** |
| richards | 4,760 | 78,791 | **16.55×** | 8,080,416 | 8,255,206 | **1.02×** | 8,087,420 | 8,334,838 | **1.03×** |
| rust-compression | 470,740 | 6,026,446 | **12.80×** | 9,147,447 | 7,011,707 | 0.77× | 9,730,437 | 13,131,575 | **1.35×** |
| rust-html-rewriter | 305,394 | 5,040,894 | **16.51×** | 10,553,225 | 7,597,691 | 0.72× | 10,960,618 | 12,714,323 | **1.16×** |
| rust-json | 104,926 | 1,854,372 | **17.67×** | 8,351,198 | 7,431,357 | 0.89× | 8,495,824 | 9,344,817 | **1.10×** |
| rust-protobuf | 57,536 | 1,060,249 | **18.43×** | 8,608,270 | 6,868,050 | 0.80× | 8,688,714 | 7,992,614 | 0.92× |
| shootout-ackermann† | 23,101 | 314,912 | **13.63×** | 4,458,921 | 6,765,212 | 1.52×† | 4,488,771 | 7,102,361 | 1.58×† |
| shootout-base64 | 21,580 | 298,391 | **13.83×** | 8,430,290 | 6,977,796 | 0.83× | 8,458,034 | 7,276,483 | 0.86× |
| shootout-ctype | 20,964 | 279,688 | **13.34×** | 16,446,425 | 5,007,782 | 0.30× | 16,473,374 | 5,289,421 | 0.32× |
| shootout-ed25519 | 156,501 | 2,433,365 | **15.55×** | 13,583,478 | 5,230,330 | 0.39× | 13,763,442 | 7,665,066 | 0.56× |
| shootout-fib2 | 18,230 | 212,775 | **11.67×** | 5,724,262 | 6,513,958 | **1.14×** | 5,745,389 | 6,729,126 | **1.17×** |
| shootout-gimli | 876 | 14,187 | **16.20×** | 8,133,057 | 7,880,831 | 0.97× | 8,134,416 | 7,895,305 | 0.97× |
| shootout-heapsort | 5,076 | 86,637 | **17.07×** | 8,598,014 | 8,052,023 | 0.94× | 8,605,567 | 8,142,038 | 0.95× |
| shootout-keccak | 5,474 | 380,074 | **69.44×** | 6,919,520 | 6,919,802 | **1.00×** | 6,930,333 | 7,302,845 | **1.05×** |
| shootout-matrix | 21,734 | 295,168 | **13.58×** | 8,174,559 | 6,793,478 | 0.83× | 8,206,978 | 7,091,562 | 0.86× |
| shootout-memmove | 21,483 | 296,703 | **13.81×** | 8,765,509 | 8,662,075 | 0.99× | 8,795,167 | 8,959,784 | **1.02×** |
| shootout-minicsv | 2,509 | 51,982 | **20.72×** | 9,835,814 | 9,312,911 | 0.95× | 9,839,446 | 9,367,089 | 0.95× |
| shootout-nestedloop | 17,287 | 213,887 | **12.37×** | 5,455,583 | 8,833,641 | **1.62×** | 5,477,592 | 9,049,712 | **1.65×** |
| shootout-random | 16,812 | 208,387 | **12.40×** | 6,958,251 | 4,386,574 | 0.63× | 6,979,176 | 4,600,728 | 0.66× |
| shootout-ratelimit | 21,337 | 286,524 | **13.43×** | 8,432,882 | 8,439,496 | **1.00×** | 8,460,619 | 8,720,769 | **1.03×** |
| shootout-seqhash | 22,724 | 326,013 | **14.35×** | 5,474,540 | 5,569,024 | **1.02×** | 5,504,819 | 5,891,837 | **1.07×** |
| shootout-sieve | 17,134 | 212,461 | **12.40×** | 8,258,961 | 6,859,184 | 0.83× | 8,280,332 | 7,075,041 | 0.85× |
| shootout-switch | 83,036 | 2,714,850 | **32.70×** | 9,216,732 | 9,144,320 | 0.99× | 9,322,398 | 11,853,010 | **1.27×** |
| shootout-xblabla20 | 21,572 | 299,854 | **13.90×** | 8,396,266 | 8,334,348 | 0.99× | 8,426,501 | 8,648,516 | **1.03×** |
| shootout-xchacha20 | 22,847 | 293,346 | **12.84×** | 8,333,080 | 8,104,947 | 0.97× | 8,365,725 | 8,394,754 | **1.00×** |

**†** `shootout-ackermann` is bimodal due to deep-recursion branch
predictor behavior. A 10-run resample put tier-2 V2 and LLVM JIT O2
medians within 0.2% of each other (6,035k vs 6,021k µs). **The apparent
1.52× WT win is a sampling artifact**; the 3-run median in this table is
kept for consistency but should not be cited.

### Aggregates

| Aggregate | IL speedup | WT speedup | TtV speedup |
|---|---:|---:|---:|
| **Geometric mean** | **14.62×** | **0.86×** | **1.07×** |
| Arithmetic mean | 16.45× | 0.97× | 1.44× |
| Max | 69.44× (keccak) | 1.62× (nestedloop) | 15.52× (noop) |
| Min | 2.15× (hashset) | 0.30× (ctype) | 0.32× (ctype) |
| IR+T2 wins (≥ 1.00×) | **33 / 33** | 8 / 33 | **21 / 33** |

### Per-metric leaderboards

**Work time — tier-2 V2 wins (8 kernels, ackermann excluded):**
`shootout-nestedloop` **1.62×**, `shootout-fib2` **1.14×** (verified
stable, 7 runs), `gcc-loops` **1.03×** (new at O2), `richards` **1.02×**,
`shootout-seqhash` **1.02×**, `shootout-keccak` **1.00×**,
`shootout-ratelimit` **1.00×**, `shootout-memmove` **0.99×** (parity).

**Work time — LLVM JIT wins decisively (the "loss cluster"):**
`shootout-ctype` 0.30×, `blind-sig` 0.34×, `shootout-ed25519` 0.39×,
`shootout-random` 0.63×, `hashset` 0.65×.

**Work time — small slowdowns (investigated below):**
`rust-html-rewriter` 0.72×, `rust-compression` 0.77×, `rust-protobuf`
0.80×, `shootout-base64` 0.83×, `shootout-matrix` 0.83×, `shootout-sieve`
0.83×, `pulldown-cmark` 0.88×, `rust-json` 0.89×.

---

## Observations

### 1. TtV near parity, WT ~14% gap

Tier-2 V2 reaches 1.07× TtV geomean vs LLVM JIT O2 on this suite; WT
trails at 0.86×. The TtV win comes from the massive IL advantage
(14.62×) — tier-2 V2 starts executing immediately while LLVM JIT
blocks on whole-module compile. The WT gap narrows to ~0.96× if the
five loss-cluster kernels are excluded.

### 2. Instantiation latency is a structural win (33/33)

Every kernel instantiates faster under tier-2 V2 because the arm doesn't
pay LLVM compile cost up front. Worst case is `hashset` at 2.15×, best
is `shootout-keccak` at 69.44×.

### 3. Tier-2 does not beat LLVM at the same opt level

Every apparent WT win is explained without positing "tier-2 optimizes
better than LLVM":

a. **Tier-1 already matches LLVM O2** on scalar-tight kernels where
   vectorization finds nothing — `shootout-nestedloop`, `richards`,
   `shootout-seqhash`, `shootout-ratelimit`, `shootout-keccak`. The
   "win" belongs to tier-1, not tier-2.
b. **Mini-module narrowing** — `shootout-fib2` 1.14× (7-run stable): the
   synthesized batch module is small enough that LLVM's
   inliner/SCCP reach a tighter fixed point than on the full module.
c. **O3→O2 flip** — `gcc-loops` crossed to 1.03× only because LLVM JIT
   dropped from O3 to O2.
d. **`shootout-ackermann`** is variance, not signal (see table note).

### 4. Loss cluster root cause: singleton-batch fragmentation

The five loss-cluster kernels (ctype, blind-sig, ed25519, random,
hashset) are not caused by scalar filter rejection, v128 workloads, or
O3-only optimizations. The O3→O2 shift did not move their numbers at
all. Two experiments identified the real cause:

**Experiment A — tier-1-only baseline.** Running each kernel with
`WASMEDGE_TIER2_ENABLE=0` showed 4/5 loss-cluster kernels were *already
faster* on tier-1 than with tier-2 enabled: ctype 1.98× faster, ed25519
1.59×, hashset 1.40×, blind-sig 1.24×. Tier-2 was actively regressing
them.

**Experiment B — batch telemetry.** At `SPDLOG_LEVEL=info`, the tier-2
manager logs each batch as "starting batch compile for func N with M
functions". For ed25519, 11/11 batches were singletons; for ctype, 5/6.

**Mechanism.** Small scalar primitives (ed25519 field ops, ctype byte
predicates) get hot first — their call counts exceed their callers'
counts, so they trip the tier-up threshold before parents do. They get
promoted **alone** into singleton batches. Result:

- **No intra-batch inlining** — the primitive is alone in its LLVM
  module; LLVM's inliner has nothing to fold it into.
- **No cross-batch inlining** — the callers (still in tier-1, or later
  in separate batches) reach the primitive through `FuncTable[idx]` →
  `fN_fwd_thunk` / `fN_t1_thunk`, which is an indirect call through an
  ORC-resolved symbol. LLVM never sees caller and callee in the same
  module.

A 5-instruction primitive that LLVM JIT would inline into its hot caller
becomes a full indirect call + ABI marshalling on every invocation.
That cost is the loss cluster.

### 5. Small slowdowns have two distinct causes

(See next section for the investigation.) The 8 kernels in the 0.72–0.89
WT range split cleanly into two categories with different root causes;
they should not be treated as a single phenomenon.

### 6. `shootout-nestedloop` is really a tier-1 win, not a tier-2 win

`WASMEDGE_TIER2_ENABLE=0` for nestedloop gives the same WT as with
tier-2 enabled. The 1.62× "tier-2" headline is tier-1 outrunning LLVM JIT
O2 on a kernel where the outer loop dominates and LLVM O2 can't find
vectorization opportunities the scalar tier-1 path misses.

### 7. Post-opt pipeline tuning is not the lever

Because singleton fragmentation is the dominant effect, tweaking the
`default<O2>` post-processing pass pipeline on the batch module cannot
help the loss cluster — the batch module contains one function that has
no callees to inline in the first place. The lever is upstream of the
pass pipeline: change **which functions end up in a batch**.

### 8. `shootout-ackermann` variance caveat

10 runs per arm under three configs (tier-1-only, tier-2, LLVM JIT O2)
showed bimodal distributions that overlap. Medians are within 0.2% across
configs; tier-2 V2 has no measurable effect on ackermann. Any future
comparison involving ackermann must use at least 10 runs or exclude it.

### 9. No correctness regressions

All 33 kernels produce golden-matching output under tier-2 V2. No core
dumps, no hangs, no mismatches under IR JIT O2.

---

## Small slowdowns investigation (2026-04-16)

The 8 small-slowdown kernels (WT 0.72–0.89× vs LLVM O2) were investigated
with the same two-experiment methodology as the loss cluster: tier-1-only
baseline and batch telemetry. Three runs per config; variance σ/µ ≤ 1.2%
so ≥3% deltas are real signal.

### Data

| kernel | tier-1 only | tier-2 V2 | LLVM O2 | **t2/t1** | LLVM/t2 | LLVM/t1 | batches (single/total) |
|---|---:|---:|---:|---:|---:|---:|---:|
| rust-html-rewriter | 8,010,913 | 10,636,403 | 7,597,691 | **1.328** | 0.71 | **0.95** | 41/67 (61%) |
| rust-protobuf | 7,247,156 | 8,509,370 | 6,868,050 | **1.174** | 0.81 | **0.95** | 53/66 (80%) |
| pulldown-cmark | 7,985,154 | 8,576,379 | 7,575,599 | **1.074** | 0.88 | **0.95** | 77/94 (82%) |
| rust-json | 7,891,552 | 8,292,597 | 7,431,357 | **1.051** | 0.90 | **0.94** | 64/80 (80%) |
| rust-compression | 10,031,505 | 9,189,998 | 7,011,707 | 0.916 | 0.76 | **0.70** | 48/65 |
| shootout-base64 | 8,416,698 | 8,481,671 | 6,977,796 | 1.008 | 0.82 | **0.83** | 3/4 |
| shootout-matrix | 8,174,695 | 8,159,875 | 6,793,478 | 0.998 | 0.83 | **0.83** | 4/6 |
| shootout-sieve | 8,118,489 | 8,274,535 | 6,859,184 | 1.019 | 0.83 | **0.85** | 2/3 |

The `t2/t1` column — whether enabling tier-2 helps or hurts relative to
tier-1-only — splits the group cleanly.

### Category A — tier-2 actively regresses; tier-1 was already near LLVM

`rust-html-rewriter`, `rust-protobuf`, `pulldown-cmark`, `rust-json`.

- Tier-1 alone runs within 5–6% of LLVM JIT O2 (`LLVM/t1` 0.94–0.95).
- Enabling tier-2 makes things **worse** by 5–33%. rust-html-rewriter
  regresses by 33%; rust-protobuf by 17%.
- Batch telemetry shows 61–82% of batches are singletons — the same
  fragmentation fingerprint as the loss cluster.

**The "tier-2 is slower than LLVM" framing is misleading for this
category. Tier-2 is dragging a near-optimal tier-1 down.** Same mechanism
as the loss cluster: every singleton batch installs a `fN_fwd_thunk` in
FuncTable, so every tier-1 call into that function pays the TLS lookup
+ arg marshal + indirect call cost. In kernels with wide call graphs
(rust-html-rewriter has 67 batches), that cost accumulates.

Fixing batch fragmentation should move these from net-negative to at
least tier-1-parity: from 0.71–0.90× up toward 0.94–0.95× (where
tier-1-only already sits).

### Category B — tier-1 is structurally slower than LLVM; tier-2 is neutral

`rust-compression`, `shootout-base64`, `shootout-matrix`, `shootout-sieve`.

- **`t2/t1 ≈ 1.00`**: within variance for base64/matrix/sieve, 8% help
  for rust-compression. Tier-2 neither helps nor hurts meaningfully.
- Tier-1 alone is 15–43% slower than LLVM JIT O2. rust-compression is
  the extreme at 0.70.
- Very few hot functions exist at all: base64 has 4 batches total,
  sieve 3, matrix 6. Tier-2 has almost nothing to work with.

**The slowdown here is a tier-1 codegen-quality problem, not a tier-2
problem.** These kernels are scalar arithmetic loops (Miller-Rabin primes
for sieve, matrix multiply, base64 byte encode, DEFLATE for
rust-compression) where LLVM O2's LoopVectorizer / unroller / SLP
vectorizer find optimizations tier-1 doesn't. The hot functions **are**
in tier-2 batches, but they're a small fraction of overall wasm work —
most of the kernel runs outside the hot loop and stays on tier-1.

Batch fragmentation fixes will **not** help this category. Closing
Category B's gap would require improving tier-1 codegen quality, or
expanding tier-2 scope past the top hot functions (but with 3–6 hot
functions, there's nothing to expand to).

### Caveats

- Only 3 runs per config. Variance is tight (σ/µ ≤ 1.2%), so deltas ≥3%
  are real signal; smaller deltas are treated as noise, which is why
  matrix (1.00), sieve (1.02), and base64 (1.01) land in the "neutral"
  column despite being just above 1.00.
- No batch-composition analysis. Singleton fragmentation's fingerprint
  matches, but this does not prove the singleton functions are on the
  hot path. Correlating singleton `funcIdx` with tier-1 call counts
  would tighten the argument.
- Category B's "tier-1 is structurally slower" diagnosis is inferred
  from kernel shape and the `t2/t1 ≈ 1.00` observation. I have not
  directly confirmed that LLVM's LoopVectorizer is the missing piece.

---

## Actionable follow-ups (prioritized)

### P1 — Fix batch fragmentation

Largest lever by far. Three sub-approaches, not mutually exclusive:

1. **Transitive scalar-callee expansion.** When assembling a batch for
   hot function `F`, walk `Call` opcodes to depth N (N=3 or 4) instead
   of depth 1. Not enough on its own (see the "callees have higher
   counts" argument below), but a building block.

2. **Parent-first promotion / delay leaf promotion.** The core fix. If
   function `G` hits the tier-up threshold but analysis shows it is
   called from `F` which has a lower-but-still-growing counter, defer
   promoting `G` until `F` is ready. Then batch them together. This
   directly attacks the root cause: leaves get promoted alone because
   their counts are higher than their callers' counts, so they always
   trip the threshold first.

3. **Cross-batch linking via a shared ORC session.** Currently every
   batch gets its own JITDylib and calls across batches bottom out in
   `fN_t1_thunk` indirection. A shared session would let a later batch
   link against an earlier batch's real symbols — still not inlined,
   but eliminates the TLS-lookup + thunk overhead per call.

Expected impact:
- Loss cluster: 0.30–0.65× → potentially 0.85–1.00× range.
- Category A small slowdowns: 0.71–0.90× → 0.94–0.95× (tier-1 parity).
- Category B: no effect.

### P2 — Inline the TLS accessor

`wasmedge_tier2_get_jit_env` is an ORC absolute symbol. LLVM's
post-opt pipeline cannot inline it. Replacing it with an `alwaysinline`
function baked into the batch module would remove ~15–25 cycles per
cross-batch call. Useful across all categories but smaller lever than P1
for fragmented workloads.

### P3 — Tier-1 codegen quality (Category B)

Not a tier-2 project but worth flagging: rust-compression, base64,
matrix, sieve show that tier-1 leaves 15–43% on the table versus LLVM
O2 on scalar arithmetic loops. Closing this would need work in the
dstogov/ir pipeline — autovectorization, more aggressive unrolling, SLP.
Out of scope for the tier-2 V2 project.

### P4 — v128 scope expansion

Previously P3. Demoted because the v128 restriction is not the
bottleneck on any kernel measured so far. Every kernel in both the loss
cluster and the small-slowdown set has its hot functions passing the
scalar filter. Worth revisiting only after P1.

### P5 — Observability

Add a `WASMEDGE_TIER2_DUMP_IR=1` path that writes each compiled batch's
`llvm::Module` to `/tmp/tier2_<kernel>_<funcIdx>.ll`. Useful when
investigating whether a specific post-opt pass is helping/hurting, and
complements the existing `spdlog::info` batch telemetry.
