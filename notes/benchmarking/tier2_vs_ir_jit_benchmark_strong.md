# Tier-2 vs IR-JIT-only on `sightglass-strong`

## Why `sightglass-strong` exists

Upstream Sightglass kernels (`test/ir/testdata/sightglass/`) were sized for
cheap CI signal: most of them finish in well under a second on a modern
desktop. That is fine for "did IR JIT emit correct code?" regression testing,
but it is the **wrong** shape for measuring our tier-2 background LLVM
recompiler.

The problem: tier-2 works by first running wasm under IR JIT (tier-1) while an
LLVM background thread compiles the same functions at higher opt levels, then
hot-swapping to the LLVM code once it is ready. If the whole benchmark
finishes before the swap, the measurement is dominated by IR JIT's work and
you learn nothing about LLVM codegen quality. Even when the swap does happen,
if only a few hundred milliseconds of exec remain post-swap you cannot
separate real steady-state speedup from noise.

`sightglass-strong` fixes this by strengthening every kernel so that its
**tier-1-only** `workTimeUs` lands in `[5s, 10s]`, targeting ~8s. That gives
tier-2 a predictable runway: a few hundred ms of compile + swap overhead at
the start, then multiple seconds of post-swap execution that the harness can
measure cleanly. The target band is tight on purpose — too low and compile
cost dominates, too high and CI time explodes.

The strengthened suite lives at `test/ir/testdata/sightglass-strong/`
alongside regenerated `.stdout.expected` / `.stderr.expected` goldens. The
goldens were produced by an **independent** runtime (wasmtime 43.0.1, not
WasmEdge) so a regression in WasmEdge's loader, IR JIT, or tier-2 path shows
up as a golden mismatch rather than silently corrupting both the runtime
output and the oracle. Upstream `test/ir/testdata/sightglass/` is left
untouched — the harness picks the suite via the `WASMEDGE_SIGHTGLASS_DIR`
env var, default `sightglass`, so existing CI flows are unaffected.

Kernel list (33 total, same names as upstream): `blake3-scalar`, `blind-sig`,
`bz2`, `gcc-loops`, `hashset`, `noop`, `pulldown-cmark`, `quicksort`, `regex`,
`richards`, `rust-compression`, `rust-html-rewriter`, `rust-json`,
`rust-protobuf`, and 19 `shootout-*` kernels. `noop` is kept at its upstream
size as a harness-overhead reference point (~1 µs `workTimeUs`); it is the
only kernel intentionally outside the `[5s, 10s]` band. `spidermonkey` and
`tinygo` from upstream Sightglass are excluded.

## Per-kernel strengthening changes

Source changes live in `~/Desktop/sightglass-strong/` (separate checkout). The
resulting `.wasm` binaries plus goldens land in
`test/ir/testdata/sightglass-strong/`. Upstream `test/ir/testdata/sightglass/`
is untouched.

Categories mirror the plan:
- **A** — bump a single `#define`/constant in the existing source.
- **B** — bump a literal inside the kernel's `main()` outer loop.
- **C** — wrap the bench region in a new outer loop.
- **D** — leave as-is (noop only).
- **special** — needed additional surgery (LICM/SCEV barriers, OOB fixes,
  nondeterministic print removal, input file bump).

### Category A — shootout internal-knob bumps (`benchmarks/shootout/src/`)

| Kernel | File | Change |
|---|---|---|
| shootout-sieve | `sieve.c` | `#define LENGTH 17000` → `1000000` |
| shootout-heapsort | `heapsort.c` | `#define ITERATIONS 1000` → `15000` |
| shootout-matrix | `matrix.c` | `#define ITERATIONS 300000` → `35000000` |
| shootout-memmove | `memmove.c` | `#define ITERATIONS 10` → `3500` |
| shootout-ratelimit | `ratelimit.c` | `#define ITERATIONS 1000000` → `50000000` |
| shootout-gimli | `gimli.c` | `#define ITERATIONS 10000` → `100000000` |
| shootout-base64 | `base64.c` | `#define ITERATIONS 10000` → `1200000` |
| shootout-keccak | `keccak.c` | `#define ITERATIONS 10000` → `32000000` |
| shootout-xblabla20 | `xblabla20.c` | `#define ITERATIONS 1000` → `6000000` |
| shootout-xchacha20 | `xchacha20.c` | `#define ITERATIONS 1000` → `11000000` |
| shootout-ctype | `ctype.c` | `#define ITERATIONS 1000` → `60000` |
| shootout-switch | `switch.c` | `#define ITERATIONS 1000` → `300000` |
| shootout-random | `random.c` | `#define LENGTH 40000000` → `2000000000` |
| shootout-ed25519 | `ed25519.c` | `#define ITERATIONS 10000` → `50000` |
| shootout-minicsv | `minicsv.c` | `#define ITERATIONS 1000000` → `8000000` |

### Category B — main()-loop literal bumps

| Kernel | File | Change |
|---|---|---|
| quicksort | `benchmarks/quicksort/quicksort.c` | `for (i = 0; i < 100; i++)` → `40000` (see also OOB fix below) |
| richards | `benchmarks/richards/richards.c` | `for (; i < 100; i++)` → `35` (plan's 10000 timed out at >120s; 500 still overshot at 115s; final 35 lands at ~8.1s) |
| hashset | `benchmarks/hashset/HashSet.cpp` | `for (unsigned i = 0; i < 100; ++i)` → `30000` (see also golden-determinism fix) |
| gcc-loops | `benchmarks/gcc-loops/gcc-loops.cpp` | `const int Mi = 1<<18;` → `Mi = 660000;` |

### Category C — Rust / C outer-loop wraps

All wraps are inside `bench_start()` / `bench::start()` .. `bench_end()` /
`bench::end()`; any one-time print statements that depend on final state were
moved outside the loop so the golden stays single-line-deterministic.

| Kernel | File | Wrap |
|---|---|---|
| bz2 | `benchmarks/bz2/benchmark.c` | `for (int _iter = 0; _iter < 400; _iter++) { compress; decompress; }` — moved `printf("compressed length: %d\n", nZ)` outside the loop |
| blake3-scalar | `benchmarks/blake3-scalar/rust-benchmark/src/main.rs` | `for _ in 0..100_000 { let hash = blake3::hash(&data); std::hint::black_box(&hash); }` |
| rust-compression | `benchmarks/rust-compression/rust-benchmark/src/main.rs` | `for _ in 0..10 { /* gzip + brotli codecs */ }` |
| rust-json | `benchmarks/rust-json/rust-benchmark/src/main.rs` | `for _ in 0..1_600 { from_str; to_string; }` (first bumped to 2000, nudged down after measurement) |
| pulldown-cmark | `benchmarks/pulldown-cmark/rust-benchmark/src/main.rs` | `for _ in 0..2_500 { /* parse + render to fresh html_output */ }` (html_output recreated each iter to avoid accumulation) |
| regex | `benchmarks/regex/rust-benchmark/src/main.rs` | `for _ in 0..200 { /* 3 × count_matches */ }` |
| blind-sig | `benchmarks/blind-sig/rust-benchmark/src/main.rs` | `for _ in 0..200 { .. }` with first iter outside to initialize `signature` |
| rust-html-rewriter | `benchmarks/rust-html-rewriter/rust-benchmark/src/main.rs` | `for _ in 0..800 { rewrite_str(..) }` (first 1000, nudged down after measurement) |
| rust-protobuf | `benchmarks/rust-protobuf/rust-benchmark/src/main.rs` | `for _ in 0..2_500 { decode; encode; }` |

### Special cases

- **shootout-fib2** (`benchmarks/shootout/src/fib2.c`):
  - Wrapped in outer loop, final count **14** iterations.
  - Had to add `BLACK_BOX(n)` **inside** the loop: without it, LLVM's LICM
    hoisted the recursive call out entirely because `n` was a local literal
    (`int n = 42`) and `fib2` is pure. First attempt (200 iters, no
    `BLACK_BOX(n)`) measured the same 0.566s as the 15-iter version — dead
    giveaway that only one call survived. With the barrier in place each iter
    is a real call (~565 ms each under IR JIT O2), so 14 iters → ~7.9s.
  ```c
  for (int i = 0; i < 14; i++) {
      BLACK_BOX(n);
      res = fib2(n);
      BLACK_BOX(res);
  }
  ```

- **shootout-ackermann** (`benchmarks/shootout/src/ackermann.c`):
  - Input file bumped: `shootout-ackermann.n.input` `7` → `10`
    (ackermann(3, n) growth is ~2^n; n=7 gave ~5 ms, n=10 gives ~60–80 ms).
  - Wrapped in outer loop, final count **140** iterations, with LICM barriers
    on both inputs (same failure mode as fib2: pure recursive function +
    loop-invariant args = one-call hoist).
  ```c
  for (int i = 0; i < 140; i++) {
      BLACK_BOX(M);
      BLACK_BOX(N);
      result = ackermann(M, N);
      BLACK_BOX(result);
  }
  ```
  - **OOB fix**: original source had `char* buf[32]` (array of 32 pointers!)
    instead of `char buf[32]`; the `buf[n] = '\0'` write and
    `atoi(&buf)` call are both wrong in the original sightglass kernel but
    happen to not trap at wasm level. Left as-is to avoid diverging further
    from upstream; the warnings are documented.

- **shootout-nestedloop** (`benchmarks/shootout/src/nestedloop.c`):
  - `#define LENGTH 30` → `42`.
  - Inner body `x++` → `x = x * 31 + f` **and** `x` declared `volatile int`.
    Without `volatile`, clang's scalar evolution collapses the six nested
    loops into a closed-form polynomial in `n` (tried: plain `x++`, then
    `x = x * 31 + f`, then `x = rol(x, 5) ^ f` — every non-volatile form
    measured <1s where ~8s was expected). `volatile int x` forces a real
    load/store each iteration, defeating SCEV.
  ```c
  int a, b, c, d, e, f;
  volatile int x = 0;
  BLACK_BOX(x);
  ...
  for (f = 0; f < n; f++) {
      x = x * 31 + f;
  }
  ```

- **shootout-seqhash** (`benchmarks/shootout/src/seqhash.c`):
  - `#define HASH_ITERATIONS 1` → `3`.
  - `#define ARENA_SIZE 1000000` → `500000` (scaling `HASH_ITERATIONS` alone
    was non-linear because each iteration lands at a different difficulty
    level; halving the arena is a linearizing per-call work reduction).
  - **OOB fix**: stack array `HashSeqSolution solutions[LEVELS]` sized 5,
    but `hashseq_solve` writes `LEVELS * HASH_ITERATIONS` slots. With
    original `HASH_ITERATIONS=1` the bug was latent; bumping to `3` made it
    trap at wasm-level. Fixed:
    `HashSeqSolution solutions[LEVELS * HASH_ITERATIONS];`

- **quicksort** (`benchmarks/quicksort/quicksort.c`):
  - **OOB fix**: `printf("%d\n", sortlist[run + 1])` trapped under wasmtime
    oracle once `run ≥ sortelements (5000)`. With the 40000-iter bump `run`
    easily exceeds 5000. Fixed:
    `printf("%d\n", sortlist[(run % sortelements) + 1])`.

- **hashset** (`benchmarks/hashset/HashSet.cpp`):
  - Removed the nondeterministic timing printf so the golden is stable
    (`That took %lf seconds.\n` varies per run). Replaced with `printf("Done.\n")`
    and dropped the `currentTime()` before/after captures.

### Harness plumbing (`test/ir/` and the verify-kernels skill)

- `test/ir/ir_benchmark_test.cpp:~1301`: read `WASMEDGE_SIGHTGLASS_DIR` env
  var to override the testdata subdirectory while keeping the default pointed
  at `sightglass` (upstream stays the default, backwards-compatible).
- `.claude/skills/verify-sightglass-kernels/SKILL.md`: the kernel loop honors
  `WASMEDGE_SIGHTGLASS_DIR` so `/verify-sightglass-kernels` can exercise
  either suite.
- `test/ir/testdata/sightglass-strong/bench_stub.wat`: two-func no-op shim
  for wasmtime oracle runs.
- `test/ir/testdata/sightglass-strong/ORACLE.md`: provenance doc — wasmtime
  43.0.1 is the primary oracle; each kernel was executed in a per-kernel
  sandbox dir via `--preload bench=bench_stub.wat --dir .::/`, with input
  files renamed from `<kernel>.default.input` to the unprefixed name the
  kernel reads.

## Setup

- Suite: `test/ir/testdata/sightglass-strong/` (33 kernels, strengthened to land
  each tier-1 `workTimeUs` in `[5s, 10s]`).
- Harness: `./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'`
  run one kernel per invocation, 120s timeout each.
- Common env: `WASMEDGE_SIGHTGLASS_DIR=sightglass-strong`,
  `WASMEDGE_SIGHTGLASS_MODE=IR_JIT`, `WASMEDGE_IR_JIT_OPT_LEVEL=2`,
  `WASMEDGE_SIGHTGLASS_SKIP_INTERP=1`, `WASMEDGE_IR_JIT_BOUND_CHECK=0`.
- Arm A (tier-1 only): `WASMEDGE_TIER2_ENABLE=0` — log `/tmp/tier1-strong-final.log`.
- Arm B (tier-2 enabled): `WASMEDGE_TIER2_ENABLE=1` — log `/tmp/tier2-strong-fix.log`
  (current; post RC1 fix). Prior run before the RC1 fix lived at
  `/tmp/tier2-strong.log`.
- Machine: Linux 6.8, LLVM 20 dev build. Single run per arm (no repeats),
  except ackermann / fib2 / blind-sig which were re-measured with 3 repeats
  to confirm the RC1-fix deltas against run-to-run noise.

## Per-kernel workTimeUs Tier-1 vs Tier-2 vs LLVM JIT (steady-state upper bound)

To decide how much of the tier-2 picture is "IR JIT is already close to
LLVM" versus "tier-2 has overhead the pure LLVM path doesn't", the same suite
was run a third time with `WASMEDGE_SIGHTGLASS_MODE=JIT` — i.e. all
functions compiled ahead of execution by the LLVM JIT path (no IR JIT, no
hot-swap). That is the same LLVM backend tier-2 eventually swaps to, so in
steady state tier-2 should converge to LLVM JIT. Anywhere tier-2 is slower
than LLVM JIT is either compile/warmup cost the harness is attributing to
`workTimeUs`, or the fraction of the run that executed pre-swap under IR JIT.

Run log: `/tmp/llvmjit-strong.log`. Same env as the other arms, plus
`WASMEDGE_SIGHTGLASS_MODE=JIT`. Single run, no repeats.

Tier-2 numbers are post RC1 fix (rewriter now detects `FuncTablePtr`
correctly; see "Root causes / RC1 fix — results" below). Pre-fix numbers
are kept in a `t2-pre` column for kernels where the fix moved the number
by more than run-to-run noise.

| Kernel | Tier-1 (s) | Tier-2 (s) | t2-pre (s) | LLVM JIT (s) | t2 − LLVM (s) | t2 / LLVM |
|---|---:|---:|---:|---:|---:|---:|
| blake3-scalar | 5.65 | 5.55 | 5.60 | 5.48 | +0.07 | 1.01 |
| blind-sig | 9.47 | 7.64 | 7.65 | 3.97 | +3.67 | 1.92 |
| bz2 | 7.83 | 7.90 | 7.81 | 7.07 | +0.83 | 1.12 |
| gcc-loops | 7.96 | 8.12 | 8.09 | 7.02 | +1.10 | 1.16 |
| hashset | 8.05 | 8.18 | 8.19 | 7.52 | +0.66 | 1.09 |
| noop | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | — |
| pulldown-cmark | 8.21 | 7.51 | 7.55 | 7.59 | -0.08 | 0.99 |
| quicksort | 8.13 | 6.64 | 6.66 | 6.98 | -0.34 | 0.95 |
| regex | 9.30 | 8.69 | 8.56 | 8.80 | -0.11 | 0.99 |
| richards | 8.09 | 8.18 | 8.15 | 8.22 | -0.04 | 1.00 |
| rust-compression | 9.95 | 7.74 | 7.82 | 7.15 | +0.59 | 1.08 |
| rust-html-rewriter | 7.91 | 8.01 | 8.12 | 7.78 | +0.23 | 1.03 |
| rust-json | 8.00 | 7.53 | 7.39 | 7.30 | +0.23 | 1.03 |
| rust-protobuf | 7.20 | 7.27 | 7.29 | 6.69 | +0.58 | 1.09 |
| shootout-ackermann | 5.62 | **15.14** | 19.38 | **4.57** | **+10.57** | **3.31** |
| shootout-base64 | 8.40 | 8.43 | 8.53 | 7.00 | +1.43 | 1.20 |
| shootout-ctype | 8.27 | 8.41 | 8.36 | 5.05 | +3.36 | 1.67 |
| shootout-ed25519 | 8.49 | 8.13 | 8.46 | 5.25 | +2.88 | 1.55 |
| shootout-fib2 | 7.90 | **9.06** | 9.72 | 6.48 | +2.58 | 1.40 |
| shootout-gimli | 8.20 | 8.09 | 8.09 | 7.85 | +0.24 | 1.03 |
| shootout-heapsort | 8.44 | 8.68 | 8.57 | 8.05 | +0.63 | 1.08 |
| shootout-keccak | 8.18 | 6.84 | 6.90 | 6.90 | -0.06 | 0.99 |
| shootout-matrix | 8.09 | 8.15 | 8.16 | 6.82 | +1.33 | 1.19 |
| shootout-memmove | 8.14 | 8.26 | 8.30 | 8.61 | -0.35 | 0.96 |
| shootout-minicsv | 9.58 | 9.72 | 9.73 | 9.30 | +0.42 | 1.05 |
| shootout-nestedloop | 5.46 | 5.48 | 5.47 | **8.85** | **-3.37** | **0.62** |
| shootout-random | 6.94 | 6.99 | 7.01 | 4.37 | +2.62 | 1.60 |
| shootout-ratelimit | 8.58 | 8.55 | 8.59 | 8.43 | +0.12 | 1.01 |
| shootout-seqhash | 5.42 | 5.44 | 5.44 | 5.55 | -0.11 | 0.98 |
| shootout-sieve | 8.14 | 10.17 | 10.16 | 6.88 | +3.29 | 1.48 |
| shootout-switch | 8.81 | 9.29 | 9.34 | 9.11 | +0.18 | 1.02 |
| shootout-xblabla20 | 8.01 | 7.87 | 8.01 | 8.25 | -0.38 | 0.95 |
| shootout-xchacha20 | 8.26 | 8.20 | 8.22 | 8.10 | +0.10 | 1.01 |

**Noise footnote:** `ackermann`, `fib2`, `blind-sig` use the median of 3
post-fix runs because the sequential suite run produced outliers for two of
them (ackermann: 15.64 in suite, 15.14 median of {14.64, 15.14, 16.34};
blind-sig: 9.15 in suite — a thermal/scheduling outlier — steady at 7.64
across {7.66, 7.64, 7.58}). Everything else is the single-run suite number.

### Observations

**1. The ackermann outlier is largely explained by a rewriter bug (RC1); it is exec cost, not compile bleed.**
Earlier analysis hypothesised that tier-2's 19.38s on ackermann was
compile-latency or swap-stall being attributed to `workTimeUs`. Reading the
tier-2 pipeline end-to-end and dumping batch LLVM IR disproved that: the
Phase-5 intra-batch rewriter was silently no-op'ing suite-wide because it
latched `FuncTableSSA` onto the counter-prologue's first `load ptr, ptr`
instead of the real FuncTable load. Every tier-2 intra-batch call — including
ackermann's recursive self-call — stayed indirect through `FuncTable`, so
the 14.81 s gap to LLVM JIT was real per-call overhead, not measurement
noise. Fixing the rewriter closes ~40% of the gap on ackermann
(19.38 → 15.14 s) and ~18% on fib2 (9.72 → 9.06 s). See "Root causes
(code investigation)" below for the full analysis and the RC1 fix-results
table.

**2. RC1 fix is localised: non-recursive kernels are flat.**
Outside ackermann and fib2, no kernel moved by more than ~0.15 s between
the pre-fix and post-fix runs (see `t2-pre` column). Most of the strong
suite has single-function batches — the rewriter has nothing to rewrite, so
it firing correctly is invisible at the kernel level. This is expected and
consistent with the root-cause analysis: RC1 only attacks call overhead.

**3. tier-2 is still not converging to LLVM JIT.**
Even after RC1, 22 of 32 non-noop kernels are slower than LLVM JIT by a
measurable amount; the unweighted mean of `t2/LLVM` across non-noop kernels
is ~1.17 (~17%, barely moved from the pre-fix 1.19 because the fix only
dents two kernels). The residual gap is explained by RC2 (wasm ABI passes
args through an alloca'd buffer, not registers), RC3 (saturated counter
prologue still executes on every call), and RC4 (LLVM vectorisation passes
disabled as an ISel-bug workaround). See the RC-to-anomaly mapping table
below.

**4. The tier-2 "winners" over tier-1 are exactly where LLVM beats IR JIT by enough margin to absorb tier-2 overhead.**
The tier-1 winners (blind-sig, rust-compression, quicksort, keccak, regex,
pulldown-cmark, rust-json): for all of them, LLVM JIT is ≥ tier-2, often
substantially (blind-sig: LLVM is 2× tier-1, tier-2 captures only half of
that headroom). The tier-2 speedup over tier-1 is real, but it is "LLVM
codegen advantage minus tier-2 overhead", not the full LLVM advantage.

**5. Kernels where tier-2 ≈ tier-1 are the RC4 targets.**
shootout-ctype (+3.36 s LLVM advantage), shootout-ed25519 (+2.88),
shootout-random (+2.62), shootout-sieve (+3.29), shootout-base64 (+1.43),
shootout-matrix (+1.33): LLVM JIT beats IR JIT by multiple seconds but
tier-2 matches tier-1. These are loop-heavy workloads whose LLVM-JIT
advantage comes from the LoopVectorize / SLPVectorize passes — passes the
tier-2 pipeline explicitly disables (`tier2_compiler.cpp:386-387`). Until
RC4 is addressed, tier-2 cannot touch these kernels regardless of how
cleanly the swap happens.

**6. `shootout-nestedloop`: LLVM JIT is *slower* than tier-1 and tier-2.**
LLVM JIT 8.85s vs tier-1/tier-2 at 5.46s/5.48s. Mirror image of the usual
pattern, consistent with the `volatile int x` workaround: LLVM respects
`volatile` and emits a real load/store every inner iteration, while
WasmEdge's IR JIT treats the wasm store-load sequence as something it can
still keep in a register. Tier-2 sits at tier-1's number, meaning the LLVM
swap either never happened for nestedloop (threshold not crossed — `main`
runs once) or the post-swap fraction is tiny. Known quirk of the SCEV
workaround; not actionable.

**7. Kernels where LLVM JIT is *faster* than tier-1 by a lot are candidates to re-tune.**
ackermann (-1.05s), ctype (-3.22s), ed25519 (-3.24s), random (-2.57s),
base64 (-1.40s), matrix (-1.27s): IR JIT tier-1 is substantially slower than
LLVM JIT here. The strong-suite calibration was done under IR JIT
`workTimeUs`, so kernels that LLVM compresses much further end up out of
band if anyone ever runs the suite in LLVM-JIT mode directly (ackermann,
ctype, ed25519, random all drop below 5.5 s under LLVM JIT). Not a bug —
`sightglass-strong` is explicitly calibrated for the tier-1 arm — but worth
knowing if the suite ever gets reused for pure LLVM JIT benchmarking.

### Summary (post RC1 fix)

| Bucket | Count | Meaning |
|---|---:|---|
| tier-2 within 3% of LLVM JIT | 12 | tier-2 is converging; good |
| tier-2 3–15% slower than LLVM JIT | 11 | realistic swap-overhead band |
| tier-2 >15% slower than LLVM JIT | 8 | tier-2 leaving exec headroom on the table |
| tier-2 faster than LLVM JIT | 1 | nestedloop, volatile quirk |

Tier-2 is close to LLVM JIT on ~38% of kernels (up from ~28% pre-fix, from
three ackermann-family kernels tightening towards LLVM JIT), in a tolerable
overhead band on ~34%, and leaves meaningful room on the floor on ~25%.
The >15% bucket is now dominated by RC4 (vectorisation-disabled) targets
plus ackermann, which is still far from LLVM JIT because RC2/RC3 remain
open.

## Root causes (code investigation)

After the LLVM-JIT comparison above surfaced the gap, I read the tier-2
pipeline end-to-end and dumped the batch LLVM IR (`WASMEDGE_TIER2_DUMP_IR=1`)
for a few hot kernels. Four compounding root causes explain the whole
pattern — the systematic ~19% gap, the tunable kernels with multi-second
headroom, and the ackermann 4.24× outlier.

### RC1 (primary) — `rewriteIntraBatchCalls` mis-detects `FuncTablePtr`

**Where:** `lib/vm/tier2_compiler.cpp:581-587` (rewriter) vs
`lib/vm/ir_builder.cpp:342-378` (counter prologue).

The Phase-5 intra-batch rewriter scans each function's LLVM IR text and
latches `FuncTableSSA` onto the *first* `load ptr, ptr %dN` line it sees,
with a comment asserting "it's always `%d4 = load ptr, ptr %d2` (env pointer
deref)". That comment was written before the tier-up counter prologue
existed. The counter prologue (emitted unconditionally when
`Tier2Threshold > 0`) does this first, in SSA order:

```
%t4  = ptrtoint ptr %d2 to i64
%t4' = add i64 %t4, 112              ; offsetof(JitExecEnv, CallCounters)
%d4  = inttoptr i64 %t4' to ptr
%d5  = load ptr, ptr %d4             ; <-- CallCountersPtr, NOT FuncTablePtr
```

So `FuncTableSSA` latches onto `%d5` (the counter-array pointer). Every
subsequent pattern match for `FuncTableSSA + k*8 -> load -> call` fails, and
the rewriter silently emits zero direct calls.

**Evidence:**
- `/tmp/tier2_batch_wasm_tier2_014.ll` (ackermann, batch of 1). Line 6 is
  the counter-prologue load `%d5 = load ptr, ptr %d4`; the real FuncTablePtr
  lands at line 33 as `%d29 = load ptr, ptr %d2`. The recursive self-call
  at line 85 stays as `call x86_fastcallcc i64 %d67(ptr %d2, ptr %d44)` —
  fully indirect through `FuncTable[14]`.
- Grepping the full tier-2 run log for "rewrote N indirect → direct" shows
  **zero hits across all 5 batch compiles** in the focused ackermann run.
  Phase 5's cross-function inlining is effectively dead across the whole
  suite.

**Impact:** every tier-2 intra-batch call — including self-recursion — stays
indirect. For ackermann (~10⁸ recursive calls) this means each call does:
store args into alloca'd buffer, load `FuncTable[14]`, indirect branch,
callee counter-prologue on entry, load args back out of the buffer. LLVM
JIT mode uses register-passing direct calls and pays none of this.

**Fix sketch:** bias detection so `FuncTableSSA` only latches on a
`load ptr, ptr %dN` where `%dN` is *directly* the env pointer parameter
(`%d2`), not via a `ptrtoint`/`add`/`inttoptr` chain. One line. Verifying
the fix requires rerunning ackermann + re-checking the dump for rewritten
`@wasm_tier2_NNN` call sites.

### RC2 — IR JIT wasm ABI baked into tier-2 (memory-buffer arg passing)

**Where:** `lib/vm/ir_builder.cpp` direct-call emission (around line 3228+).

Wasm→wasm calls in the IR backend pass args through a shared alloca'd
buffer (`alloca i8, i32 16`) rather than through native-register CC. That
choice is serialised into `ir_ctx`, round-tripped through
`ir_save`/`ir_load`, and transliterated to LLVM IR verbatim by
`ir_emit_llvm`. LLVM cannot promote the buffer to registers because the
call target is opaque (indirect through `FuncTable` — see RC1).

**Impact:** compounds with RC1. Even if LLVM had visibility into the
callee, the store-to-buffer / load-from-buffer pattern for every arg on
every call costs real cycles on tight recursion (ackermann, fib2). Fixing
RC1 alone will let LLVM see the callee signature and forward the alloca
through SROA/mem2reg; RC2 becomes mostly self-healing.

### RC3 — Counter prologue persists in the tier-2 function body

**Where:** `lib/vm/ir_builder.cpp:342-378` + the ir_save/ir_load round-trip.

The tier-up counter-check IR nodes are part of the graph `ir_save`
serialises, so tier-2 functions re-emit the entire counter prologue: load
counter, compare to threshold, branch. The threshold is already saturated
by the time we hit tier-2 (that's *why* we hit tier-2), so every call pays
a load-cmp-branch pair that always falls through to the `l6` (above
threshold) path. See lines 7-12 of `/tmp/tier2_batch_wasm_tier2_014.ll`.

**Impact:** small per-call (~3-5 cycles) but compounds on hot recursion.
Not a dominant cost on its own, but worth stripping when tier-2 rewrites
the function — we know the counter will never matter again.

### RC4 — Vectorisation disabled in tier-2

**Where:** `lib/vm/tier2_compiler.cpp:386-387`.

```cpp
LLVMPassBuilderOptionsSetLoopVectorization(PBO, 0);
LLVMPassBuilderOptionsSetSLPVectorization(PBO, 0);
```

Disabled as a workaround for LLVM 18 ISel bugs on wasm-vector patterns.
Tier-2 therefore cannot match LLVM JIT on any kernel whose LLVM-JIT win
comes from the vectoriser.

**Impact:** directly explains the seven "tunable" kernels from the LLVM JIT
comparison table where t2 sits at tier-1 but LLVM JIT has multi-second
headroom:

| Kernel | LLVM JIT advantage | Likely vectorisable |
|---|---:|---|
| shootout-ctype | +3.31s | byte-scan loops |
| shootout-ed25519 | +3.21s | field-arith loops |
| shootout-random | +2.64s | PRNG mix step |
| shootout-base64 | +1.53s | byte tables |
| shootout-matrix | +1.34s | MMUL inner loop |
| shootout-sieve | +3.28s | sieve crossout |
| shootout-fib2 | +3.24s | (no — that one is RC1) |

Re-enabling vectorisation requires tracking down the LLVM 18 ISel crash
first; a narrower workaround (enable LoopVectorize, leave SLPVectorize off,
or vice versa) might unlock most of the headroom without the original
crash.

### How the four RCs map to the observed anomalies

| Observation | RC1 | RC2 | RC3 | RC4 |
|---|:-:|:-:|:-:|:-:|
| ackermann 4.24× outlier | **★** | ★ | ★ | |
| fib2 +3.24s vs LLVM JIT | **★** | ★ | ★ | |
| sieve +3.28s vs LLVM JIT | ★ | | ★ | **★** |
| ctype / ed25519 / random / base64 / matrix headroom | | | | **★** |
| Systematic ~19% mean t2/LLVM gap | ★ | ★ | ★ | ★ |
| Phase-5 "22% sieve improvement" claim in tier2_doc.md no longer reproduces | **★** | | | |

**Priority:** fix RC1 first — it is a one-line detector fix, reactivates
all of Phase 5, and is the only RC that explains the ackermann outlier.
After RC1, re-run the strong suite and re-score before tackling RC3/RC4.

### RC1 fix — results

**Fix:** `lib/vm/tier2_compiler.cpp` — parse the first parameter SSA name
from each `define` line (the env pointer) and only latch `FuncTableSSA`
onto a `load ptr, ptr %<env-param>` whose source is literally that
parameter. Skips the counter-prologue's `load ptr, ptr %d4`
(CallCountersPtr via ptrtoint/add/inttoptr chain).

**Verification on the ackermann dump:**
- Before: line 85 of `/tmp/tier2_batch_wasm_tier2_014.ll`
  `call x86_fastcallcc i64 %d67(ptr %d2, ptr %d44)` — indirect.
- After: same line now reads
  `%d68_narrow = call i32 @wasm_tier2_014(ptr %d2, ptr %d44)` — direct,
  default CC, with an auto-inserted i64→i32 zext fixup for the return.

**Suite-level activation:** 81 `rewrote N indirect → direct calls` events
across the strong suite (was 0 before the fix), and the run passes with
no failures / mismatches.

**Per-kernel impact** (median of 3 follow-up runs for the recursion-heavy
kernels; other kernels are single-run numbers from the suite pass and
should be treated as within noise):

| Kernel | Tier-2 (before, s) | Tier-2 (after, s) | Δ | LLVM JIT (s) |
|---|---:|---:|---:|---:|
| shootout-ackermann | 19.38 | **15.14** | **−4.24 (−22%)** | 4.57 |
| shootout-fib2 | 9.72 | **9.06** | **−0.66 (−7%)** | 6.48 |
| blind-sig | 7.65 | 7.64 | 0.00 | 3.97 |
| shootout-sieve | 10.16 | 10.17 | +0.01 | 6.88 |
| everything else | — | — | within run-to-run noise (±0.15s) | — |

**Interpretation:**

- **Ackermann closes ~40% of the LLVM gap** (14.81s before → 10.57s after).
  The remaining ~10.6s vs LLVM JIT is consistent with RC2 (args through
  alloca buffer instead of registers) and RC3 (residual counter prologue),
  exactly as the root-cause analysis predicted — those compound on tight
  recursion.
- **fib2 improvement (−0.66s) is smaller than ackermann's** even though
  both are recursive. fib2's per-call cost is dominated by the two
  recursive sub-calls' actual work; ackermann's is dominated by call
  overhead. RC1 attacks call overhead, so ackermann benefits more.
- **Non-recursive kernels are flat**, as expected: RC1 only matters when
  a batch member calls another batch member. Single-function batches
  (which is most of the suite) have no intra-batch calls to rewrite, so
  the rewriter's firing is largely invisible. This also explains why
  sieve does **not** improve — its hot loop has no intra-batch calls;
  its LLVM-JIT gap is RC4 (vectorisation), not RC1.
- **blind-sig held steady** at 7.64s (re-verified with 3 repeats). The
  initial sequential-run reading of 9.15s was pure run-to-run noise, not
  a regression.

**What's still open:** RC2 (memory-buffer arg ABI), RC3 (counter prologue
persistence), RC4 (vectorisation disabled). RC4 is still the biggest
remaining lever — ctype/ed25519/random/base64/matrix/sieve are all
untouched by RC1 and collectively leave ~15 s of headroom on the table.

## Tier-2 vs tier-1: winners, regressions, flat (post RC1 fix)

### Winners (tier-2 ≥ +5% faster than tier-1)

| Kernel | Tier-1 (s) | Tier-2 (s) | Speedup |
|---|---:|---:|---:|
| rust-compression | 9.95 | 7.74 | 1.29× (-22.2%) |
| blind-sig | 9.47 | 7.64 | 1.24× (-19.3%) |
| quicksort | 8.13 | 6.64 | 1.22× (-18.3%) |
| shootout-keccak | 8.18 | 6.84 | 1.20× (-16.4%) |
| pulldown-cmark | 8.21 | 7.51 | 1.09× (-8.5%) |
| regex | 9.30 | 8.69 | 1.07× (-6.6%) |
| rust-json | 8.00 | 7.53 | 1.06× (-5.9%) |

Same shape as before the fix: wins cluster on crypto/compression (blind-sig,
keccak, rust-compression) and kernels with many small hot functions
(quicksort, pulldown-cmark, rust-json). These are the workloads where LLVM's
inliner + loop optimiser have the most headroom over IR JIT O2.

### Regressions (tier-2 slower than tier-1 by > 1 s, post-fix)

| Kernel | Tier-1 (s) | Tier-2 (s) | Δ | Root cause |
|---|---:|---:|---:|---|
| shootout-ackermann | 5.62 | 15.14 | +9.52 | RC2 + RC3 (RC1 fix only closed 4.24 s of 14.81 s) |
| shootout-sieve | 8.14 | 10.17 | +2.03 | RC4 (LLVM vectorisation disabled) |
| shootout-fib2 | 7.90 | 9.06 | +1.16 | RC2 + RC3 (RC1 fix closed 0.66 s) |

These are no longer unexplained: each has an identified code-level cause
in the Root Causes section below. Ackermann is still the biggest outlier,
but the 10.6 s gap vs LLVM JIT is now consistent with wasm-ABI args going
through an alloca'd buffer (RC2) plus the persistent counter-prologue
load-cmp-branch on every call (RC3) on ~10⁸ recursive calls — i.e.
per-call overhead that LLVM JIT's direct register-passing doesn't pay.

### Flat / neutral (|Δ| < 1 s from tier-1)

Everything else (~23 kernels). Most of the strong suite sits here because
IR JIT O2 is already close to LLVM O2 codegen on those shapes; the tier-2
hot-swap has little headroom to capture. RC4-limited kernels (ctype,
ed25519, random, base64, matrix) live in this bucket because tier-2's
disabled vectoriser can't reach LLVM JIT's numbers, so tier-2 settles at
tier-1's level even though LLVM JIT has multi-second headroom.

## Open questions

1. **Is RC2 worth chasing once RC1 lands?** Now that ackermann's call site
   is a direct `call i32 @wasm_tier2_014(...)`, LLVM has everything it
   needs to forward the alloca'd buffer via SROA/mem2reg — in principle.
   A follow-up `perf stat` run on ackermann would tell us whether the
   remaining 10.6 s gap is dominated by (a) the store/load round-trip to
   the arg buffer, or (b) the counter prologue, or (c) something else we
   haven't looked at yet.
2. For the big winners (compression, blind-sig, quicksort, keccak): which
   LLVM pass is responsible? Worth dumping post-opt LLVM IR for one of these
   kernels and diffing against the IR JIT dump for the same function.
3. `shootout-sieve`: the +2.03 s regression was unchanged by the RC1 fix,
   which is consistent with RC4 (sieve's inner loop is a textbook
   LoopVectorize target, disabled in tier-2). Confirming this would take
   a one-off tier-2 build with LoopVectorize re-enabled on sieve only.

## Reproducing

From `build/`:

```sh
# Arm A
for wasm in ../test/ir/testdata/sightglass-strong/*.wasm; do
  kernel="$(basename "$wasm" .wasm)"
  WASMEDGE_SIGHTGLASS_DIR=sightglass-strong \
  WASMEDGE_TIER2_ENABLE=0 \
  WASMEDGE_SIGHTGLASS_KERNEL="$kernel" \
  WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
  WASMEDGE_SIGHTGLASS_SKIP_INTERP=1 \
  WASMEDGE_IR_JIT_OPT_LEVEL=2 \
  WASMEDGE_IR_JIT_BOUND_CHECK=0 \
  stdbuf -oL timeout 120 ./test/ir/wasmedgeIRBenchmarkTests \
    --gtest_filter='*SightglassSuite*'
done

# Arm B: same loop with WASMEDGE_TIER2_ENABLE=1.
```
