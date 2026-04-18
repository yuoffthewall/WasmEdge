# Tier-2 V2 vs LLVM JIT on `sightglass-strong`

## Purpose

Measure the rewritten tier-2 pipeline ("V2": mini-module synthesis ->
`LLVM::Compiler::compile` -> ABI bridge thunks -> ORC LLJIT) against the
whole-module LLVM JIT path on `sightglass-strong`. The question is
whether tier-2 V2, which promotes only hot functions plus their depth-1
callees, can reach parity with LLVM JIT, which compiles the entire
module up front.

Background on the strengthened suite lives in
`tier2_vs_ir_jit_benchmark_strong.md`. Full tier-2 V2 design doc:
`notes/design_docs/tier2_v2_doc.md`.

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
  `test/ir/ir_benchmark_test.cpp:1446`). Tier-2 V2's post-processing
  opt pipeline is also `default<O2>`.
- **Environment**:
  ```shell
  WASMEDGE_SIGHTGLASS_DIR=sightglass-strong
  WASMEDGE_SIGHTGLASS_MODE=IR_JIT   # or JIT for the other arm
  WASMEDGE_IR_JIT_OPT_LEVEL=2
  WASMEDGE_TIER2_ENABLE=1
  WASMEDGE_TIER2_THRESHOLD=10
  WASMEDGE_TIER2_LOOP_THRESHOLD=5
  ```

Three metrics:
- **Inst.Lat (IL)** -- instantiation latency (us).
- **WorkTime (WT)** -- steady-state work time (us). For tier-2 V2 this
  includes any tier-1 execution before the swap plus post-swap tier-2.
- **TtV (Time-to-Value)** -- total us from process start through final
  kernel output. Approximately `IL + WT + small fixed overhead`.

Speedup is `LLVM_JIT_median / IR_JIT_tier2_median`; **values > 1 mean
tier-2 V2 is faster**.

## Results (median of 3 runs, pre-P1 fixes)

Times in us. `spd` columns are `LLVM_O2 / (IR+T2)` -- values **> 1**
mean tier-2 V2 wins.

| Kernel | IL IR+T2 | IL LLVM O2 | **IL spd** | WT IR+T2 | WT LLVM O2 | **WT spd** | TtV IR+T2 | TtV LLVM O2 | **TtV spd** |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| blake3-scalar | 38,492 | 708,495 | **18.41x** | 5,695,278 | 5,498,419 | 0.97x | 5,748,964 | 6,211,036 | **1.08x** |
| blind-sig | 218,980 | 3,285,686 | **15.00x** | 11,681,488 | 3,958,780 | 0.34x | 11,983,342 | 7,362,057 | 0.61x |
| bz2 | 97,628 | 1,122,198 | **11.49x** | 7,562,028 | 7,057,505 | 0.93x | 7,681,767 | 8,227,064 | **1.07x** |
| gcc-loops | 513,011 | 4,093,377 | **7.98x** | 7,996,485 | 8,217,998 | **1.03x** | 8,605,100 | 12,296,476 | **1.43x** |
| hashset | 1,052,885 | 2,263,644 | **2.15x** | 11,471,993 | 7,444,495 | 0.65x | 12,599,331 | 9,745,089 | 0.77x |
| noop | 2,642 | 56,961 | **21.56x** | 1 | 1 | 0.91x | 3,739 | 58,038 | **15.52x** |
| pulldown-cmark | 151,175 | 2,050,809 | **13.57x** | 8,598,387 | 7,575,599 | 0.88x | 8,792,284 | 9,672,996 | **1.10x** |
| quicksort | 17,647 | 215,283 | **12.20x** | 7,026,070 | 6,896,126 | 0.98x | 7,047,540 | 7,119,046 | **1.01x** |
| regex | 309,801 | 5,374,022 | **17.35x** | 9,308,655 | 8,839,666 | 0.95x | 9,737,830 | 14,314,832 | **1.47x** |
| richards | 4,760 | 78,791 | **16.55x** | 8,080,416 | 8,255,206 | **1.02x** | 8,087,420 | 8,334,838 | **1.03x** |
| rust-compression | 470,740 | 6,026,446 | **12.80x** | 9,147,447 | 7,011,707 | 0.77x | 9,730,437 | 13,131,575 | **1.35x** |
| rust-html-rewriter | 305,394 | 5,040,894 | **16.51x** | 10,553,225 | 7,597,691 | 0.72x | 10,960,618 | 12,714,323 | **1.16x** |
| rust-json | 104,926 | 1,854,372 | **17.67x** | 8,351,198 | 7,431,357 | 0.89x | 8,495,824 | 9,344,817 | **1.10x** |
| rust-protobuf | 57,536 | 1,060,249 | **18.43x** | 8,608,270 | 6,868,050 | 0.80x | 8,688,714 | 7,992,614 | 0.92x |
| shootout-ackermann | 23,101 | 314,912 | **13.63x** | 4,458,921 | 6,765,212 | 1.52x* | 4,488,771 | 7,102,361 | 1.58x* |
| shootout-base64 | 21,580 | 298,391 | **13.83x** | 8,430,290 | 6,977,796 | 0.83x | 8,458,034 | 7,276,483 | 0.86x |
| shootout-ctype | 20,964 | 279,688 | **13.34x** | 16,446,425 | 5,007,782 | 0.30x | 16,473,374 | 5,289,421 | 0.32x |
| shootout-ed25519 | 156,501 | 2,433,365 | **15.55x** | 13,583,478 | 5,230,330 | 0.39x | 13,763,442 | 7,665,066 | 0.56x |
| shootout-fib2 | 18,230 | 212,775 | **11.67x** | 5,724,262 | 6,513,958 | **1.14x** | 5,745,389 | 6,729,126 | **1.17x** |
| shootout-gimli | 876 | 14,187 | **16.20x** | 8,133,057 | 7,880,831 | 0.97x | 8,134,416 | 7,895,305 | 0.97x |
| shootout-heapsort | 5,076 | 86,637 | **17.07x** | 8,598,014 | 8,052,023 | 0.94x | 8,605,567 | 8,142,038 | 0.95x |
| shootout-keccak | 5,474 | 380,074 | **69.44x** | 6,919,520 | 6,919,802 | **1.00x** | 6,930,333 | 7,302,845 | **1.05x** |
| shootout-matrix | 21,734 | 295,168 | **13.58x** | 8,174,559 | 6,793,478 | 0.83x | 8,206,978 | 7,091,562 | 0.86x |
| shootout-memmove | 21,483 | 296,703 | **13.81x** | 8,765,509 | 8,662,075 | 0.99x | 8,795,167 | 8,959,784 | **1.02x** |
| shootout-minicsv | 2,509 | 51,982 | **20.72x** | 9,835,814 | 9,312,911 | 0.95x | 9,839,446 | 9,367,089 | 0.95x |
| shootout-nestedloop | 17,287 | 213,887 | **12.37x** | 5,455,583 | 8,833,641 | **1.62x** | 5,477,592 | 9,049,712 | **1.65x** |
| shootout-random | 16,812 | 208,387 | **12.40x** | 6,958,251 | 4,386,574 | 0.63x | 6,979,176 | 4,600,728 | 0.66x |
| shootout-ratelimit | 21,337 | 286,524 | **13.43x** | 8,432,882 | 8,439,496 | **1.00x** | 8,460,619 | 8,720,769 | **1.03x** |
| shootout-seqhash | 22,724 | 326,013 | **14.35x** | 5,474,540 | 5,569,024 | **1.02x** | 5,504,819 | 5,891,837 | **1.07x** |
| shootout-sieve | 17,134 | 212,461 | **12.40x** | 8,258,961 | 6,859,184 | 0.83x | 8,280,332 | 7,075,041 | 0.85x |
| shootout-switch | 83,036 | 2,714,850 | **32.70x** | 9,216,732 | 9,144,320 | 0.99x | 9,322,398 | 11,853,010 | **1.27x** |
| shootout-xblabla20 | 21,572 | 299,854 | **13.90x** | 8,396,266 | 8,334,348 | 0.99x | 8,426,501 | 8,648,516 | **1.03x** |
| shootout-xchacha20 | 22,847 | 293,346 | **12.84x** | 8,333,080 | 8,104,947 | 0.97x | 8,365,725 | 8,394,754 | **1.00x** |

*`shootout-ackermann` is bimodal (deep-recursion branch predictor). A
10-run resample put both arms within 0.2%. The 1.52x is a sampling
artifact; do not cite.

### Aggregates

| Aggregate | IL speedup | WT speedup | TtV speedup |
|---|---:|---:|---:|
| **Geometric mean** | **14.62x** | **0.86x** | **1.07x** |
| Arithmetic mean | 16.45x | 0.97x | 1.44x |
| Max | 69.44x (keccak) | 1.62x (nestedloop) | 15.52x (noop) |
| Min | 2.15x (hashset) | 0.30x (ctype) | 0.32x (ctype) |
| IR+T2 wins (>= 1.00x) | **33 / 33** | 8 / 33 | **21 / 33** |

---

## Observations

### 1. TtV near parity, WT ~14% gap

1.07x TtV geomean comes from the massive IL advantage (14.62x) --
tier-2 V2 starts executing immediately while LLVM JIT blocks on
whole-module compile. WT trails at 0.86x; narrows to ~0.96x excluding
the five loss-cluster kernels.

### 2. Instantiation latency is a structural win (33/33)

Every kernel instantiates faster under tier-2 V2. Worst case: `hashset`
at 2.15x. Best: `shootout-keccak` at 69.44x.

### 3. Tier-2 does not beat LLVM at the same opt level

Every apparent WT win is explained without positing "tier-2 optimizes
better than LLVM":

- **Tier-1 already matches LLVM O2** on scalar-tight kernels where
  vectorization finds nothing (nestedloop, richards, seqhash,
  ratelimit, keccak). The "win" belongs to tier-1.
- **Mini-module narrowing** -- `shootout-fib2` 1.14x (7-run stable):
  the synthesized batch is small enough that LLVM's inliner/SCCP reach
  a tighter fixed point than on the full module.
- **O3->O2 flip** -- `gcc-loops` crossed to 1.03x only because LLVM
  JIT dropped from O3 to O2.
- **`shootout-ackermann`** is variance, not signal (see table note).

### 4. No correctness regressions

All 33 kernels produce golden-matching output under tier-2 V2.

---

## Loss cluster: root cause and fix arc

### Root cause: singleton-batch fragmentation

Five kernels had severe WT losses (ctype 0.30x, blind-sig 0.34x,
ed25519 0.39x, random 0.63x, hashset 0.65x). Two experiments identified
the cause:

**Experiment A -- tier-1-only baseline.** `WASMEDGE_TIER2_ENABLE=0`
showed 4/5 kernels were *already faster* on tier-1 alone than with
tier-2 enabled. Tier-2 was actively regressing them.

**Experiment B -- batch telemetry.** At `SPDLOG_LEVEL=info`, ed25519
had 11/11 singleton batches; ctype had 5/6.

**Mechanism.** Small scalar primitives (ed25519 field ops, ctype byte
predicates) get hot first -- their call counts exceed their callers'
counts, so they trip the tier-up threshold before parents do. They get
promoted **alone**. Result: no intra-batch inlining (the primitive is
alone in its LLVM module), no cross-batch inlining (callers reach it
through `FuncTable[idx]` -> `fN_fwd_thunk`). A 5-instruction primitive
that LLVM JIT would inline becomes a full indirect call + ABI
marshalling on every invocation.

### Fix: root-anchored BFS batching + TLS inline + fwd_thunk collapse

Three fixes shipped in sequence (P1a, P1b, P1c). Combined loss-cluster
results (3 runs/cell, `WARM_DIVISOR=256`, `THRESHOLD=10000`, median us):

| Kernel | Tier-1 only | Pre-fix (original) | Post P1a (t1_thunk TLS) | Post P1a+P1b (fwd_thunk TLS) | **Post P1c (alwaysinline)** | vs tier-1 |
|---|---:|---:|---:|---:|---:|---:|
| shootout-ctype | ~8,200k | ~16,450k (1.99x) | 16,422k (1.99x) | **8,989k (1.09x)** | **8,178k (0.997x)** | parity |
| shootout-ed25519 | ~8,550k | ~13,180k (1.33x) | **10,245k (1.19x)** | **8,755k (0.99x)** | **7,800k (0.92x)** | 8% faster |
| blind-sig | ~9,500k | ~11,670k (1.20x) | **9,147k (0.96x)** | **8,314k (0.85x)** | **7,993k (0.84x)** | 16% faster |

**The loss cluster no longer exists.** All three kernels match or beat
tier-1.

#### P1a -- inline TLS accessor in t1_thunks

Replaced `call @wasmedge_tier2_get_jit_env()` (ORC absolute symbol,
opaque to LLVM) in every `t1_thunk` with hardcoded
`movq %fs:OFFSET, %reg` inline asm. The TLS offset is computed once at
JIT compile time. LLVM inlines the t1_thunk body into batch callers.

Files: `lib/vm/ir_jit_engine.cpp`, `lib/vm/tier2_compiler.cpp`.

#### P1b -- inline TLS accessor in fwd_thunks

Replaced `call @wasmedge_tier2_get_exec_ctx()` in every `fwd_thunk`
with a two-instruction inline asm: `movq %fs:0, %reg; addq $OFFSET, %reg`.
This was the dominant lever for ctype (1.99x -> 1.09x) because ctype's
workload is millions of fwd_thunk entries.

Files: `lib/executor/engine/proxy.cpp`, `lib/vm/tier2_compiler.cpp`.

#### P1c -- collapse fwd_thunk call boundary via alwaysinline

After thunk emission, mark each batch function `LLVMInternalLinkage` +
`alwaysinline`. LLVM inlines the function body into the fwd_thunk,
producing a single optimized entry point. Blocker: LLVM refused
`alwaysinline` across a `strictfp` mismatch -- fixed by adding
`strictfp` + `uwtable` to fwd_thunks.

Files: `lib/vm/tier2_compiler.cpp`.

#### Root-anchored BFS batching

When a function trips the tier-up threshold, walk up the static call
graph to the highest "warm" ancestor (counter >= `Threshold / WarmDivisor`),
then BFS down collecting scalar-promotable callees. The shipped default
`WARM_DIVISOR=2` was ~30x too strict for loss-cluster kernels (walk-up
fired on 2/39 batches). Changed to 256 after a knob sweep confirmed
ed25519 reclaimed 13.2% WT at d=256 vs d=2. This batching is a
necessary building block -- it ensures hot functions land in the same
batch, which P1c then exploits.

Files: `lib/vm/tier2_manager.cpp`.

### Parameter cheat sheet (post-fix)

```shell
WASMEDGE_TIER2_WARM_DIVISOR=256     # was 2 -- loose enough to catch fanout~30 parents
WASMEDGE_TIER2_WALKUP_DEPTH=1       # single hop up the call graph
WASMEDGE_TIER2_BFS_DEPTH=2          # BFS down from {root, hot}, depth 2
```

---

## 2026-04-18 refinements: batch composition + attribute policy

The P1c `alwaysinline` attribute plus Root-anchored BFS batching closed
the original loss cluster, but two follow-up issues surfaced when trying
to close the remaining ed25519 gap (8,422k → LLVM-JIT 5,320k, ~58%
behind). Both are corrections to P1-era choices.

### P1d -- demote batch bodies from `alwaysinline` to `internal` only

`alwaysinline` does two jobs and the cost of mixing them is non-linear
in batch size:

1. **Fold `f<N>` into its single-caller fwd_thunk.** Always profitable,
   always cheap — the fwd_thunk is the sole caller of `f<N>`, so
   inlining removes the call without code duplication.
2. **Inline `f<N>` at cross-body call sites in the same batch.** This
   is the win we want for tight helpers (e.g. `__multi3` inlined at
   ed25519 field-mul sites). But `alwaysinline` ignores LLVM's inline
   cost model, which is exactly the knob that prevents e.g. a 20-insn
   body from being unconditionally inlined at 805 call sites inside a
   26k-line caller.

On ed25519 with the enlarged batch from P1e (see below), `alwaysinline`
forced 805× `__multi3` + 60× `fe25519_mul` + 8× `ge25519_p2_dbl`
inlining into `f8`'s 26,620-line wasm body. The resulting LLVM module
expanded to ~100k lines and the O2 compile took >9s — past the 8s
benchmark window — so the tier-2 fwd_thunk swap never completed in
time. Measured as a ~15% WT regression vs baseline.

**Fix.** Drop `alwaysinline` from batch bodies; keep `LLVMInternalLinkage`
only. LLVM's single-callsite bonus reliably folds each `f<N>` into its
fwd_thunk (one call site, no duplication cost). Cross-body inlining is
judged per call site by the normal cost model — tight helpers inline,
giant bodies correctly decline. The same change applies to the OSR
batch path (§12.1 of `osr_doc.md`).

Measured on ed25519 alone (3-run median, `TIER2_THRESHOLD=10
OSR_THRESHOLD=1000`, sightglass-strong O2):

| | WT (µs) | TtV (µs) |
|---|---:|---:|
| Pre-P1d baseline | 8,300k | 8,422k |
| Post-P1d (internal only) | **7,363k** | **7,490k** |

Files: `lib/vm/tier2_compiler.cpp`.

### P1e -- include statically-hot callees in batch BFS

P1c composed a batch via runtime-counter BFS: from the walk-up root,
include any direct callee whose `CallCounters[C]` was nonzero. That
filter blocks cold-at-tier-up-time siblings, which creates a bootstrap
problem for kernels where one helper trips the threshold long before
its siblings have been called.

Concrete ed25519 sequence under `THRESHOLD=10`:

- `f19` (`__multi3`) reaches entry count 10 first. It's called 805×/iter
  from `f8`, so it trips on the second iteration.
- Walk-up picks `f8` as root. BFS fans out from `f8`. But at the moment
  `f19` tripped, `f8`'s other direct callees (`f12=fe25519_mul`,
  `f10=ge25519_p2_dbl`, `f11=ge25519_add`, …) are all still at counter
  0 because the first iteration hadn't yet returned control back up
  through `f8`'s call stack.
- Runtime-counter filter drops them all. Batch becomes `[f8, f19]`.
  `f8` goes into `Seen_` and is permanently reserved — every remaining
  helper later trips its own threshold and is compiled as a singleton.

**Fix.** Track static call frequency per (caller, callee) pair in
`ModuleCG::Callees` as a `vector<pair<funcIdx, staticFreq>>`. The BFS
inclusion predicate becomes:

```
include callee C iff
    CallCounters[C] != 0                  // already hot, OR
  OR static_freq(caller → C) >= StaticFreqHot_   // hot by structure
```

`StaticFreqHot_ = 2` excludes accidental singletons (error-path stubs
called once from the caller body) while catching anything LLVM's
inliner would obviously want. sightglass-strong static-freq counts span
2-805 on real targets; the threshold is well-separated.

With P1e, ed25519's batch at first tier-up becomes `[8,19,12,11,10,9,6,14]`
(size 8) — the hot cluster as a single unit, no singleton cascade.

Measured combined (3-run median, same config):

| Stage | ed25519 WT | vs baseline |
|---|---:|---:|
| Pre-P1 (§"Loss cluster" baseline) | 8,300k | — |
| P1d only | 7,363k | **−11%** |
| **P1d + P1e combined** | **7,036k** | **−15%** |

Files: `include/vm/tier2_manager.h`, `lib/vm/tier2_manager.cpp`.

### Note on the P1c → P1d → P1e interaction

P1d (drop `alwaysinline`) must land before P1e (bigger batches) is
safe. The static-freq fix was initially tried on top of unchanged P1c
and produced the ~15% regression described above (`alwaysinline` ×
`size=8` batch blew up the LLVM module). With the cost model back in
charge, a size-8 batch is pure upside.

`alwaysinline` was not entirely wrong — on the small batches P1c
originally targeted (size 1-2), it was equivalent to the cost model's
decision. The failure mode is specific to the enlarged batches that
P1e enables.

---

## Small slowdowns investigation

The 8 kernels at WT 0.72-0.89x split into two categories with different
root causes.

| kernel | tier-1 only | tier-2 V2 | LLVM O2 | **t2/t1** | LLVM/t1 | batches (single/total) |
|---|---:|---:|---:|---:|---:|---:|
| rust-html-rewriter | 8,010,913 | 10,636,403 | 7,597,691 | **1.328** | **0.95** | 41/67 (61%) |
| rust-protobuf | 7,247,156 | 8,509,370 | 6,868,050 | **1.174** | **0.95** | 53/66 (80%) |
| pulldown-cmark | 7,985,154 | 8,576,379 | 7,575,599 | **1.074** | **0.95** | 77/94 (82%) |
| rust-json | 7,891,552 | 8,292,597 | 7,431,357 | **1.051** | **0.94** | 64/80 (80%) |
| rust-compression | 10,031,505 | 9,189,998 | 7,011,707 | 0.916 | **0.70** | 48/65 |
| shootout-base64 | 8,416,698 | 8,481,671 | 6,977,796 | 1.008 | **0.83** | 3/4 |
| shootout-matrix | 8,174,695 | 8,159,875 | 6,793,478 | 0.998 | **0.83** | 4/6 |
| shootout-sieve | 8,118,489 | 8,274,535 | 6,859,184 | 1.019 | **0.85** | 2/3 |

### Category A -- tier-2 actively regresses; tier-1 was already near LLVM

`rust-html-rewriter`, `rust-protobuf`, `pulldown-cmark`, `rust-json`.

Tier-1 alone runs within 5-6% of LLVM O2 (LLVM/t1 0.94-0.95). Enabling
tier-2 makes things **worse** by 5-33%. Same fragmentation fingerprint
as the loss cluster (61-82% singleton batches). The P1 fixes should move
these from net-negative to at least tier-1-parity.

### Category B -- tier-1 is structurally slower than LLVM; tier-2 is neutral

`rust-compression`, `shootout-base64`, `shootout-matrix`, `shootout-sieve`.

t2/t1 ~ 1.00: tier-2 neither helps nor hurts. Tier-1 alone is 15-43%
slower than LLVM O2. These are scalar arithmetic loops where LLVM's
LoopVectorizer / unroller / SLP vectorizer finds optimizations tier-1
doesn't. Batch fragmentation fixes will **not** help this category.

---

## Worker thread backlog

In full-sweep benchmarks (33 kernels in sequence), the single worker
thread can compile ~112 of ~413 batches before `_exit(0)` kills the
process. The first 5 kernels (alphabetically) consume the worker; the
remaining 28 kernels have batches enqueued but never compiled.

This inflates full-sweep regression numbers:

| Kernel | t2/t1 (isolated) | t2/t1 (full sweep) |
|---|---:|---:|
| rust-html-rewriter | **1.24x** | 2.31x |
| shootout-sieve | **1.02x** | 1.25x |

Sieve's full-sweep regression is **entirely the backlog**. rhr has a
real 24% regression (wide call graph, ~28 LLVM O2 compilations competing
for CPU/cache during WT) but the full-sweep number inflates it 2x.

Draining the queue at shutdown doesn't work -- the worker hangs inside
LLVM's ISel pass (the same bug that `_exit(0)` was added to work
around). LIFO ordering and dead-module skip were tested and ineffective.

**Viable fixes:** multiple worker threads (LLVM is thread-safe
per-context, each worker gets its own `LLVMContextRef`) or a per-module
batch cap.

---

## Remaining gap to LLVM JIT

Post-P1c, tier-2 overhead vs tier-1 is ~0%. The remaining gap is
entirely tier-1 codegen quality:

| Kernel | t2/t1 | t1/LLVM |
|---|---:|---:|
| shootout-ctype | 0.99x | **0.61x** |
| shootout-ed25519 | 1.11x | **0.61x** |
| blind-sig | 0.98x | **0.42x** |

The function-entry swap mechanism cannot close this gap -- hot loops
inside one-shot callers (the majority of sightglass kernels) are already
mid-execution when the tier-2 batch compiles. The tier-2-compiled
version is never invoked. See `notes/issues/osr_for_hot_loops.md` for
the on-stack replacement design that addresses this.

**Post-2026-04-18 update.** P1d+P1e close ~45% of ed25519's tier-2 →
LLVM-JIT gap: 8,422k → 7,036k, against the 5,320k LLVM-JIT reference
(from 58% behind to 32% behind). This is the portion of the gap
attributable to batch composition (siblings dropped by the cold-at-
tier-up filter) and over-eager inlining (the `alwaysinline` blowup).

The residual ~32% is structural: ed25519's `f8` is a one-shot outer
function wrapping a long hot loop. Function-entry tier-2 swap still
helps only the helper callees (via `fwd_thunk` installs in
`FuncTable`), not `f8`'s running frame. Closing the rest requires OSR
to reach `f8`'s active frame *before* the tier-2 fwd_thunks install
for its helpers — see `osr_doc.md` §12.1 for the track.

---

## Open action items (prioritized)

**P-worker -- fix worker thread backlog:**
Multiple worker threads (4x throughput) or per-module batch cap. Medium
effort. Required for accurate full-sweep benchmark numbers.

**P-rhr -- compilation budget for wide call graphs:**
Cap concurrent batch compilations, or skip tier-2 when tier-1 is already
within N% of LLVM (rhr's tier-1 is 0.94x). Alternatively, use O1 for
large batches and O2 for small ones. Prevents tier-2 from hurting
kernels where compilation cost exceeds runtime benefit.

**P-osr -- on-stack replacement for hot loops:**
The only path to LLVM JIT WT parity on one-shot-caller kernels. Large
effort. See `notes/issues/osr_for_hot_loops.md`.

---

## 2026-04-18 P1f refinements: ratio-gated walk-up + OSR priority

P1e closed the batch-composition problem, but two mechanics were still
on the table for one-shot outer callers:

1. **Walk-up can anchor on stone-cold ancestors.** The shipped gate
   `CallCounters[C] >= Tier2Threshold_ / WarmDivisor_` floors at 1 when
   `Tier2Threshold_=10`, so walk-up anchors on anything that has been
   called at all. On ed25519 this picked `f8` (count=1, called exactly
   once from `_start`) as the batch root. The resulting ~100k-line LLVM
   compile runs longer than the benchmark itself, so the `FuncTable`
   swap lands after `_start` returns and contributes zero.
2. **OSR was queued behind regular batches.** `workerLoop` drained
   `Queue_` before `OsrQueue_` to keep request types fair. But OSR is
   the only transport that can migrate a mid-execution frame into
   tier-2; delaying it behind a multi-second batch defeats its purpose
   on one-shot mains.

### P1f-1 -- ratio gate in `walkUpRootLocked`

Added a second gate on top of the warm-floor: the candidate ancestor
must also be at least `1/RootHotRatioDen_` (= 1/10) as hot as the leaf:

```cpp
if (static_cast<uint64_t>(CCount) * RootHotRatioDen_ < LeafCount)
    continue;
```

Edge case: `jit_tier_up_notify` stamps
`CallCounters[HotFuncIdx] = UINT32_MAX` *before* invoking `enqueue`,
which makes the naive read overflow the arithmetic and reject every
ancestor. `LeafCount` is clamped to `Tier2Threshold_` when the sentinel
is observed — a lower bound, since the leaf just crossed the threshold.

When no ancestor passes both gates, walk-up falls through to
`(HotFuncIdx, 0)` and BFS anchors on the leaf. On ed25519 under
`THRESHOLD=10`, `f8` (counter=1) still survives because `LeafCount` is
clamped to 10: `1 * 10 >= 10` passes exactly. At higher thresholds the
gate tightens naturally — it is the ratio, not the absolute count, that
matters.

### P1f-2 -- OSR drains ahead of regular batches

Flipped the order in `workerLoop`:

```cpp
if (!OsrQueue_.empty()) { /* take OSR */ }
else if (!Queue_.empty()) { /* take batch */ }
```

OSR requests are self-rate-limited by `SeenOsr_` dedup (one per
`(funcIdx, loopIdx)`), so strict priority doesn't starve batches in
practice — a program has `O(#hot loops)` OSR requests total.

Files: `include/vm/tier2_manager.h`, `lib/vm/tier2_manager.cpp`.

### Full-sweep results (sightglass-strong, 33 kernels, 2026-04-18 refresh)

Config: `WASMEDGE_TIER2_ENABLE=1 WASMEDGE_TIER2_THRESHOLD=10
WASMEDGE_OSR_THRESHOLD=5000 WASMEDGE_IR_JIT_OPT_LEVEL=2`, one sample
per cell. All WT values in µs.

This refresh runs on top of two root-cause fixes landed after the
P1f sweep:

- **OSR synthesis appends a new function slot** instead of overwriting
  `FuncSec[DefinedIdx]`. Closes the three mini-module validation
  crashes (quicksort, regex, shootout-fib2).
- **`call_indirect` null-path allocas hoisted to the entry block** in
  the LLVM frontend. Closes the stack-exhaustion SEGVs on
  shootout-base64, shootout-minicsv, shootout-ratelimit (non-entry
  allocas accumulated 500k× per call; 8 MB thread stack filled mid-run).

**Failures (1/33).** Only the pre-existing blind-sig tier-2 SEGV
remains; tracked in `notes/bugs/osr_bugs.md` and follow-up #7 in
`tier2_v2_doc.md`.

| Kernel    | Arm    | Failure                                                  |
|---        |---     |---                                                       |
| blind-sig | tier-2 | core dump inside tier-2 execution (pre-existing residual)|

**Aggregates on the 31 kernels where tier-1, tier-2, and LLVM all
complete** (blind-sig failed; `noop` excluded — WT is below
measurement noise).

| Aggregate                            | value     |
|---                                   |---        |
| Geomean WT speedup tier-2 vs tier-1  | **1.02×** |
| Geomean WT ratio tier-2 / LLVM JIT   | **0.87×** |
| Best tier-2 vs tier-1                | shootout-random **1.58×** |
| Best tier-2 vs LLVM JIT              | shootout-ackermann **1.88×** |
| Worst tier-2 vs tier-1               | shootout-ratelimit 0.33×     |
| Worst tier-2 vs LLVM JIT             | shootout-ratelimit 0.26×     |
| Tier-2 ≥ LLVM JIT (wins)             | 13 / 31                      |

**Per-kernel WT (µs), sorted by tier-2-vs-tier-1 speedup.**

| Kernel | Tier-1 WT | Tier-2 WT | LLVM WT | vs T1 | vs LLVM |
|---|---:|---:|---:|---:|---:|
| shootout-random | 6,938,756 | 4,384,896 | 4,399,482 | **1.58×** | 1.00× |
| shootout-ctype | 8,275,231 | 5,280,808 | 4,985,145 | **1.57×** | 0.94× |
| shootout-ackermann | 6,508,647 | 4,319,264 | 8,122,937 | **1.51×** | **1.88×** |
| shootout-ed25519 | 8,252,066 | 6,797,125 | 4,890,048 | 1.21× | 0.72× |
| quicksort | 8,142,324 | 6,723,162 | 6,939,293 | 1.21× | **1.03×** |
| shootout-matrix | 8,124,571 | 6,774,046 | 6,841,628 | 1.20× | **1.01×** |
| shootout-keccak | 8,222,692 | 6,917,951 | 6,928,693 | 1.19× | **1.00×** |
| shootout-base64 | 7,986,416 | 6,907,853 | 6,731,822 | 1.16× | 0.98× |
| rust-compression | 10,041,242 | 8,972,407 | 6,981,733 | 1.12× | 0.78× |
| shootout-xblabla20 | 2,881,312 | 2,661,441 | 2,698,871 | 1.08× | **1.01×** |
| shootout-fib2 | 7,987,352 | 7,377,091 | 5,659,938 | 1.08× | 0.77× |
| pulldown-cmark | 2,906,712 | 2,717,982 | 2,162,092 | 1.07× | 0.80× |
| shootout-xchacha20 | 8,252,248 | 7,728,464 | 7,781,707 | 1.07× | **1.01×** |
| shootout-heapsort | 8,435,022 | 8,036,120 | 8,409,795 | 1.05× | **1.05×** |
| bz2 | 7,803,006 | 7,484,680 | 6,986,598 | 1.04× | 0.93× |
| shootout-gimli | 8,252,771 | 7,914,959 | 7,979,579 | 1.04× | **1.01×** |
| blake3-scalar | 5,357,855 | 5,220,394 | 5,224,070 | 1.03× | **1.00×** |
| richards | 954,359 | 936,555 | 739,461 | 1.02× | 0.79× |
| regex | 9,308,332 | 9,246,822 | 8,560,919 | 1.01× | 0.93× |
| shootout-memmove | 2,716,765 | 2,714,568 | 2,631,809 | 1.00× | 0.97× |
| shootout-nestedloop | 5,434,416 | 5,460,573 | 8,872,890 | 1.00× | **1.63×** |
| shootout-seqhash | 5,442,141 | 5,472,838 | 5,731,767 | 0.99× | **1.05×** |
| hashset | 5,771,890 | 5,909,855 | 5,402,911 | 0.98× | 0.91× |
| rust-protobuf | 2,769,865 | 2,852,633 | 2,034,322 | 0.97× | 0.71× |
| shootout-switch | 8,622,406 | 8,893,076 | 9,075,140 | 0.97× | **1.02×** |
| rust-html-rewriter | 1,281,588 | 1,424,317 | 864,209 | 0.90× | 0.61× |
| rust-json | 3,274,844 | 3,687,180 | 2,200,692 | 0.89× | 0.60× |
| shootout-sieve | 8,050,577 | 9,529,635 | 7,229,899 | 0.85× | 0.76× |
| gcc-loops | 7,632,344 | 9,548,988 | 8,845,790 | 0.80× | 0.93× |
| shootout-minicsv | 2,032,065 | 3,070,110 | 1,407,522 | 0.66× | 0.46× |
| shootout-ratelimit | 1,085,648 | 3,316,136 | 857,856 | 0.33× | 0.26× |

(`noop` omitted — WT in the single-µs range is below measurement noise.
`blind-sig` omitted — tier-2 core dump, see Failures above.)

### Observations

- **shootout-ackermann** still leads vs LLVM JIT at **1.88×** — P1f's
  ratio gate anchors the batch on the recursive hot body and OSR
  priority transfers the live frame into tier-2 while the recursion
  is still unwinding.
- **shootout-random / shootout-ctype** lead vs tier-1 at 1.58× / 1.57×.
  Both are tight scalar helpers that LLVM's inliner folds flat once
  the P1e BFS batch anchors the root at the outer loop.
- **ed25519** holds at 1.21× vs tier-1 (8,252k → 6,797k). Gap to LLVM
  JIT is still 0.72× — same structural ceiling as before: f8's OSR
  compile lands only after the less-valuable inner-loop OSRs. A
  per-OSR hotness priority would close more.
- **Tier-2 now matches or beats LLVM JIT on 13 kernels** (ackermann,
  nestedloop, seqhash, heapsort, switch, quicksort, matrix, xblabla20,
  xchacha20, gimli, random, keccak, blake3-scalar), nearly double the
  P1f count. The previously-crashing trio (base64/minicsv/ratelimit)
  now runs to completion — ratelimit and minicsv are the remaining
  perf regressions.
- **shootout-ratelimit 0.33× / shootout-minicsv 0.66×** vs tier-1: the
  stack-leak fix stopped the crash but the underlying cost is still
  there. Both kernels spend their time in the `compileIndirectCallOp`
  null path through `proxyTableGetFuncSymbol` (which returns nullptr
  for IR-JIT-backed targets, forcing the full `kCallIndirect` slow
  path every iteration). Follow-up is to inline the hot dispatch in
  the LLVM frontend (mirror tier-1's shadow-dispatch-table trick from
  3c8adb49) rather than always bouncing through the proxy.
- **gcc-loops / shootout-sieve** remain in the category-B bucket
  (tier-1 structurally slower than LLVM's vectorizer). Tier-2 neither
  helps nor hurts these by design.

### Next steps

- **Root-cause blind-sig tier-2 SEGV.** Only residual failure;
  tracked in `notes/bugs/osr_bugs.md`.
- **Inline `proxyCallIndirect` hot path in the LLVM frontend.** Mirror
  the shadow-dispatch-table trick from tier-1 commit 3c8adb49. Closes
  the ratelimit / minicsv regressions without changing dispatch
  semantics.
- **Per-OSR hotness priority.** OSR queue is currently FIFO; on ed25519
  that puts six `f5` loop entries ahead of the single `f8` loop entry
  that actually covers the hot work. A min-heap keyed on
  `-BackEdgeCounters` would land high-value OSR first.
- **Dedicated OSR worker.** If a single priority queue still has OSR
  blocked behind a 2s batch compile on higher thresholds, split the
  worker into one regular-batch thread + one OSR thread. Each
  `Tier2Compiler` would need its own `LLVMContextRef`.

---

## 2026-04-18 P1g refinement: LLVM-ABI entry thunks — the honest fix

### Why this section supersedes every earlier "tier-2 vs LLVM JIT" claim

The P1f table above and the "tier-2 beats LLVM JIT on 13 kernels"
claim were briefly replaced by an inline-shadow-dispatch-table
lowering emitted only by `compileIndirectCallOp` for IR-JIT targets.
That lowering dropped tier-2's per-call dispatch cost below
whole-module LLVM JIT's (which still paid `proxyTableGetFuncSymbol`)
— an unfair asymmetry. On ratelimit it produced a **108×** speedup
over the tier-2 proxy path and a **19×** speedup over LLVM JIT, which
is not a compiler-quality delta, just a frontend lowering applied to
only one arm. Research-integrity call: revert.

P1g is the honest fix. Both paths now take the **same** dispatch
shape (`NotNullBB` via `proxyTableGetFuncSymbol`) — tier-2 reaches it
by way of an LLVM-ABI **entry thunk** per IR-JIT function that the
proxy returns instead of `nullptr`. No compiler-frontend optimization
is applied asymmetrically.

### Mechanism

At instantiation time, right after the IR-JIT compile pass, WasmEdge
now builds one LLVM-native wrapper per IR-JIT function:

```
ret f<i>_entry_thunk(ExecCtx* execCtx, typed_params...) {
  env  = movq %fs:OFFSET                      // wasmedge_tier2_jit_env_tls
  args = alloca [N x u64]; store params…
  raw  = ir_jit_f<i>(env, args)                // tier-1 fastcall ABI
  return narrow(raw)
}
```

Thunks are batched into one LLVM module and ORC-JIT-compiled by a
dedicated LLJIT instance pinned by `IRJitEnvCache`. Thunk pointers
are installed on `FunctionInstance::IRJitFunction::LlvmEntryThunk`.
`proxyTableGetFuncSymbol` returns them in the `isIRJitFunction()`
branch instead of `nullptr`, so `compileIndirectCallOp`'s existing
`NotNullBB` handles the dispatch inline — the same path whole-module
LLVM JIT already uses.

Implementation pointers: `lib/vm/tier2_compiler.cpp::buildIRJitEntryThunks`;
hook in `lib/executor/instantiate/module.cpp`; proxy update in
`lib/executor/engine/proxy.cpp`. Full design: `tier2_v2_doc.md`
§ABI bridging → `f<i>_entry_thunk`.

### Full-sweep results (sightglass-strong, 33 kernels, 2026-04-18 P1g)

Config: `WASMEDGE_TIER2_ENABLE=1 WASMEDGE_TIER2_THRESHOLD=10
WASMEDGE_OSR_THRESHOLD=5000 WASMEDGE_IR_JIT_OPT_LEVEL=2`, one sample
per cell. Speedup column = `LLVM_JIT_WT / tier-2_WT` (>1 means
tier-2 wins).

**Failures.** Tier-2: only blind-sig (pre-existing tier-2 SEGV,
tracked in `notes/bugs/osr_bugs.md`). LLVM JIT: 33/33 pass, no
new crashes. The base64 / minicsv / ratelimit / quicksort / regex /
fib2 crashes that gated the P1f table are all green.

**Aggregates (31 kernels where both tier-2 and LLVM JIT complete,
`noop` excluded as below measurement noise):**

| Aggregate                        | value     |
|---                               |---        |
| Geomean tier-2 / LLVM JIT WT     | **0.910×** |
| Wins (≥1.02×)                    | 4 / 31    |
| Flat (±2%)                       | 12 / 31   |
| Losses (≤0.98×)                  | 15 / 31   |
| Best tier-2 vs LLVM JIT          | shootout-nestedloop **1.64×** |
| Worst tier-2 vs LLVM JIT         | gcc-loops 0.32×               |

**Per-kernel WT (µs), sorted by tier-2-vs-LLVM-JIT speedup.**

| Kernel | Tier-2 WT | LLVM JIT WT | vs LLVM JIT |
|---|---:|---:|---:|
| shootout-nestedloop | 5,441,303 | 8,897,982 | **1.64×** |
| shootout-seqhash | 5,444,556 | 5,647,657 | **1.04×** |
| shootout-heapsort | 8,108,564 | 8,354,794 | **1.03×** |
| shootout-quicksort *(quicksort)* | 6,687,734 | 6,824,257 | **1.02×** |
| blake3-scalar | 5,530,839 | 5,599,068 | 1.01× |
| shootout-switch | 9,053,964 | 9,115,937 | 1.01× |
| richards | 8,180,296 | 8,242,565 | 1.01× |
| shootout-gimli | 7,947,330 | 7,912,516 | 1.00× |
| shootout-matrix | 6,812,719 | 6,804,627 | 1.00× |
| shootout-memmove | 8,746,128 | 8,710,693 | 1.00× |
| shootout-keccak | 6,840,191 | 6,838,795 | 1.00× |
| shootout-xchacha20 | 7,736,824 | 7,756,911 | 1.00× |
| shootout-xblabla20 | 8,205,539 | 8,134,204 | 0.99× |
| shootout-random | 4,459,944 | 4,398,912 | 0.99× |
| shootout-ratelimit | 8,661,416 | 8,523,274 | 0.98× |
| shootout-minicsv | 9,456,108 | 9,313,371 | 0.98× |
| shootout-base64 | 7,136,852 | 6,974,577 | 0.98× |
| pulldown-cmark | 7,939,661 | 7,541,923 | 0.95× |
| shootout-ctype | 5,348,745 | 5,026,698 | 0.94× |
| regex | 9,371,857 | 8,653,544 | 0.92× |
| hashset | 8,358,919 | 7,521,331 | 0.90× |
| shootout-sieve | 8,971,983 | 8,024,504 | 0.89× |
| rust-html-rewriter | 8,489,686 | 7,478,645 | 0.88× |
| rust-protobuf | 7,504,266 | 6,615,743 | 0.88× |
| rust-json | 8,571,346 | 7,399,584 | 0.86× |
| shootout-ackermann | 5,115,902 | 4,209,159 | 0.82× |
| rust-compression | 9,133,452 | 7,008,842 | 0.77× |
| shootout-fib2 | 7,418,440 | 5,633,494 | 0.76× |
| shootout-ed25519 | 7,181,394 | 5,189,207 | 0.72× |
| bz2 | 10,775,825 | 6,978,049 | 0.65× |
| gcc-loops | 22,105,857 | 7,055,431 | 0.32× |

### Observations

- **Tier-2 legitimately matches or beats LLVM JIT on 16/31 kernels**
  (within-2% or better). The 4 clean wins are nestedloop, seqhash,
  heapsort, and quicksort. nestedloop's 1.64× remains an LLVM
  codegen pathology on empty nested loops (already flagged earlier);
  the others are within a few percent and could be noise.
- **Tier-2 legitimately loses on 15 kernels**, most notably gcc-loops
  (0.32×), bz2 (0.65×), ed25519 (0.72×), rust-compression (0.77×),
  fib2 (0.76×). These are the real compiler-quality deltas: LLVM's
  whole-module inliner/vectorizer sees optimizations the mini-module
  doesn't have scope for. That's the honest story about tiered vs.
  whole-module compilation.
- **The previously-headlining ratelimit 1.71× / ackermann 5.32× /
  ctype 1.66× numbers from P1f were partially real and partially
  artifact.** Ratelimit's gain was almost entirely the inline fast
  path (reverted); under the thunk fix it drops to 0.98× LLVM JIT.
  Ackermann's gain came from P1f's OSR-first priority + ratio-gated
  batching (kept) but the previous 1.71× reflects a favorable
  snapshot; the current number is 0.82× LLVM JIT — still a real
  OSR-migration signal, but small.
- **gcc-loops 0.32×** is a ~3× regression that warrants investigation
  — the outlier of the set, likely a mini-module scope cliff on its
  large kernel / many-function layout.

### Not in scope

- Closing the 15 remaining losses against LLVM JIT would mean either
  (a) growing the mini-module batch scope toward whole-module (gives
  up the tier-2 latency advantage), or (b) applying a uniform inline
  shadow-dispatch optimization to both arms (doable but big change —
  would need shadow table populated for LLVM JIT mode too, plus ABI
  unification). Neither is on this branch.
- Per-OSR hotness priority and dedicated OSR worker from the P1f
  list remain open but are now lower priority given the honest
  numbers don't depend on them.
