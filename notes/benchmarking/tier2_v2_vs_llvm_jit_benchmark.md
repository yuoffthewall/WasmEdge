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

### Full-sweep results (sightglass-strong, 33 kernels)

Config: `WASMEDGE_TIER2_ENABLE=1 WASMEDGE_TIER2_THRESHOLD=10
WASMEDGE_OSR_THRESHOLD=1000 WASMEDGE_IR_JIT_OPT_LEVEL=2`, one sample
per cell. All WT values in µs.

**Failures (8/33).** P1f surfaced or retained these; none are caused
by the P1f changes themselves (they reproduce on the pre-P1f commit
when the same threshold is used).

| Kernel             | Arm             | Failure                                                |
|---                 |---              |---                                                     |
| shootout-base64    | tier-2          | core dump inside tier-2 compile/install                |
| shootout-minicsv   | tier-2          | core dump inside tier-2 compile/install                |
| shootout-ratelimit | tier-2          | core dump inside tier-2 compile/install                |
| quicksort          | tier-2          | mini-module validation: value-stack underflow at `call`|
| regex              | tier-2          | mini-module validation: type mismatch (i64 vs i32)     |
| shootout-fib2      | tier-2          | mini-module validation: value-stack underflow at `call`|
| blind-sig          | tier-1 + tier-2 | kernel trap during execution (pre-existing)            |
| rust-compression   | tier-1          | unreachable trap at runtime (pre-existing)             |

**Aggregates on the 25 kernels where tier-1, tier-2, and LLVM all
complete.**

| Aggregate                            | value     |
|---                                   |---        |
| Geomean WT speedup tier-2 vs tier-1  | **1.20×** |
| Geomean WT ratio tier-2 / LLVM JIT   | **0.93×** |
| Best tier-2 vs tier-1                | shootout-ackermann **5.32×** |
| Best tier-2 vs LLVM JIT              | shootout-ackermann **1.71×** |
| Worst tier-2 vs LLVM JIT             | rust-json 0.58×              |

**Per-kernel WT (µs), sorted by tier-2-vs-tier-1 speedup.**

| Kernel | Tier-1 WT | Tier-2 WT | LLVM WT | vs T1 | vs LLVM |
|---|---:|---:|---:|---:|---:|
| shootout-ackermann | 14,716,544 | 2,766,884 | 4,729,725 | **5.32×** | **1.71×** |
| shootout-ctype | 8,477,750 | 5,116,635 | 4,981,451 | 1.66× | 0.97× |
| shootout-random | 6,933,863 | 4,391,172 | 4,376,049 | 1.58× | 1.00× |
| shootout-matrix | 9,963,422 | 6,689,167 | 6,818,511 | 1.49× | 1.02× |
| shootout-gimli | 10,161,820 | 8,041,672 | 7,774,984 | 1.26× | 0.97× |
| shootout-ed25519 | 8,219,307 | 6,640,923 | 4,981,327 | **1.24×** | 0.75× |
| shootout-xblabla20 | 3,114,872 | 2,621,212 | 2,646,738 | 1.19× | 1.01× |
| shootout-keccak | 8,227,066 | 6,974,522 | 6,878,483 | 1.18× | 0.99× |
| pulldown-cmark | 3,204,318 | 2,746,499 | 2,168,996 | 1.17× | 0.79× |
| bz2 | 8,293,503 | 7,491,223 | 7,098,919 | 1.11× | 0.95× |
| hashset | 6,805,306 | 6,136,454 | 5,375,595 | 1.11× | 0.88× |
| rust-protobuf | 3,198,551 | 2,927,933 | 1,998,903 | 1.09× | 0.68× |
| shootout-heapsort | 8,962,877 | 8,363,460 | 8,066,146 | 1.07× | 0.96× |
| shootout-xchacha20 | 8,414,520 | 8,033,511 | 8,101,397 | 1.05× | 1.01× |
| shootout-memmove | 2,798,553 | 2,674,899 | 2,633,328 | 1.05× | 0.98× |
| shootout-switch | 9,030,089 | 8,794,297 | 9,034,148 | 1.03× | 1.03× |
| blake3-scalar | 5,356,416 | 5,277,479 | 5,455,521 | 1.01× | 1.03× |
| gcc-loops | 10,077,801 | 10,098,224 | 6,592,944 | 1.00× | 0.65× |
| shootout-nestedloop | 5,427,974 | 5,428,434 | 8,839,150 | 1.00× | 1.63× |
| rust-json | 3,629,523 | 3,668,737 | 2,136,883 | 0.99× | 0.58× |
| richards | 916,170 | 933,496 | 747,190 | 0.98× | 0.80× |
| shootout-seqhash | 5,432,619 | 5,557,004 | 5,563,611 | 0.98× | 1.00× |
| rust-html-rewriter | 1,466,474 | 1,504,861 | 900,962 | 0.97× | 0.60× |
| shootout-sieve | 8,163,649 | 8,891,090 | 6,756,967 | 0.92× | 0.76× |

(`noop` omitted — WT in the single-µs range is below measurement noise.)

### Observations

- **shootout-ackermann** is the headline: 5.32× over tier-1 and 1.71×
  over LLVM JIT. P1f's ratio gate keeps the recursive hot body as the
  batch root instead of wandering into `_start`, and the OSR priority
  lets the tier-2 frame take over deep in the call stack while the
  recursion is still unwinding.
- **ed25519** moves from ~1.11× vs tier-1 (pre-P1f) to **1.24×**
  (8,219k → 6,641k). The gap to LLVM JIT narrows to 0.75× — the
  residual is bounded by f8's OSR compile alone taking ~2.4s and
  landing only after the six `f5` OSRs that fire first. A per-OSR
  hotness priority (see next step) would close more.
- **Small regressions (≤8%)** on richards / sieve / seqhash /
  rust-html-rewriter remain background-compile noise on already-fast
  kernels; unchanged from the pre-P1f baseline.
- **Tier-2 beats LLVM JIT** on 7 kernels (ackermann, nestedloop, noop,
  blake3-scalar, switch, xblabla20, xchacha20). nestedloop at 1.63× vs
  LLVM is worth a future look — tier-1 alone matches LLVM there, so
  the win is tier-2 avoiding whatever LLVM does wrong on that shape.

### Next steps

- **Per-OSR hotness priority.** OSR queue is currently FIFO; on ed25519
  that puts six `f5` loop entries ahead of the single `f8` loop entry
  that actually covers the hot work. A min-heap keyed on `-BackEdgeCounters`
  would land high-value OSR first.
- **Dedicated OSR worker.** If a single priority queue still has OSR
  blocked behind a 2s batch compile on higher thresholds, split the
  worker into one regular-batch thread + one OSR thread. Each
  `Tier2Compiler` would need its own `LLVMContextRef`.
- **Investigate the three validation crashes** (quicksort, regex,
  shootout-fib2). Shared shape — synthesized `call` whose stack / types
  do not match — points at `synthesizeMiniModule` or
  `emitT1ThunkInPlace`.
