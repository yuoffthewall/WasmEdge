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

Kernel list (39 total, same names as upstream): `blake3-scalar`, `blind-sig`,
`bz2`, `gcc-loops`, `hashset`, `noop`, `pulldown-cmark`, `quicksort`, `regex`,
`richards`, `rust-compression`, `rust-html-rewriter`, `rust-json`,
`rust-protobuf`, `tinygo-regex`, and 19 `shootout-*` kernels are strengthened
to land in `[5s, 10s]`. The following sit in the suite at **upstream size**
on purpose:

- `noop` — harness-overhead reference point (~1 µs `workTimeUs`); intentionally
  outside the band.
- `spidermonkey-json`, `spidermonkey-markdown`, `spidermonkey-regex`,
  `tinygo-json`, `image-classification` — strengthening was attempted but
  produced tier-2/tier-1 ratios within 0.05× of 1.0× (and often *below* 1.0×;
  see [Why five kernels were left at upstream size](#why-five-kernels-were-left-at-upstream-size)
  below). They are kept at upstream size as **negative-result evidence**:
  documented but excluded from the aggregates.

The `splay` and `sqlite3` kernels from `test/ir/testdata/sightglass-strong/`
are also in the directory but are excluded from the active strengthening list
because of separate IR JIT or tier-2 bugs not in scope for this pass; see the
respective bug notes.

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
| tinygo-regex | `benchmarks/tinygo/regex/main.go` | `for i := 0; i < 7; i++ { emails = countMatches(...); uris = ...; ips = ... }` (TinyGo `opt=2 -gc=leaking`; tier-1 ~1.2 s × 7 ≈ 8.5 s) |

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

### Why five kernels were left at upstream size

Five upstream-size kernels are intentionally retained without strengthening:
`spidermonkey-json`, `spidermonkey-markdown`, `spidermonkey-regex`,
`tinygo-json`, and `image-classification`. Each was strengthened to land in
`[5s, 10s]`, measured against tier-2 (regular) and tier-2+OSR, and reverted
because the tier-2/tier-1 ratio came in within ±0.05× of flat — often *below*
flat. Single-run WTs (Release IR JIT O2, `WASMEDGE_TIER2_THRESHOLD=10`,
`WASMEDGE_OSR_THRESHOLD=5000`):

| Strengthened kernel | T1 (µs) | T2 (µs) | T2+OSR (µs) | T1/T2 | T1/(T2+OSR) |
|---|---:|---:|---:|---:|---:|
| spidermonkey-json (ITERS=100) | 6,861,438 | 6,841,687 | 7,679,136 | 1.00× | 0.89× |
| spidermonkey-markdown (ITERS=150) | 5,839,787 | 6,263,446 | 6,547,835 | 0.93× | 0.89× |
| spidermonkey-regex (ITERS=350) | 8,105,837 | 8,747,908 | 7,884,831 | 0.93× | 1.03× |
| tinygo-json (ITERS=120) | 7,895,398 | 7,844,385 | 7,826,273 | 1.01× | 1.01× |
| image-classification (ITERS=3500) | 5,067,486 | 5,059,485 | 4,937,057 | 1.00× | 1.03× |
| **tinygo-regex (ITERS=7)** | 8,468,501 | 6,185,582 | 6,231,380 | **1.37×** | **1.36×** |

`tinygo-regex` is the only one of the six newly added kernels where the
strengthened workload produces a real tier-2 win, so it is the only one
retained at strengthened size. The other five would inflate the suite's
geomean toward 1.0× without telling us anything new — they are documented
here as negative-result evidence and left at upstream size in
`test/ir/testdata/sightglass-strong/`.

Why each flat kernel resists tier-2 (root causes, in plain words):

- **image-classification** — host-bound. The bench region wraps a single
  `wasi_nn::compute(context)` call; the host plugin (OpenVINO) does the
  matrix work in native AVX. Tier-2 only recompiles wasm, and the wasm
  portion is microseconds next to milliseconds of host work, so there is
  nothing left to optimize.
- **spidermonkey-{json,markdown,regex}** — interpreter-shaped wasm. The
  kernel is the SpiderMonkey C++ engine compiled to wasm with the SM JIT
  tier disabled, so JS runs through SpiderMonkey's bytecode interpreter:
  a giant `clang -O3`'d dispatch switch. The hot inner loop is opcode-fetch
  + dispatch, bottlenecked by branch prediction and instruction fetch
  rather than codegen quality. Tier-2's wasm→x86 LLVM pass cannot recover
  the C++ semantics from already-O3'd wasm bytecode; inlining,
  vectorization, and loop choices are baked in at the C++→wasm step.
  `JSON.parse` / `marked.parse` / regex matching all live inside the
  interpreted path or in SpiderMonkey-internal C++ that the same logic
  applies to. Tier-2 compile/swap overhead also eats most of the small
  steady-state delta — SpiderMonkey is ~14,000 wasm functions per kernel.
- **tinygo-json** — reflection-heavy wasm. `encoding/json.Unmarshal`
  dispatches every field decoder through a `call_indirect` resolved at
  runtime from `reflect.Type` tables. Neither IR JIT nor tier-2 LLVM can
  devirtualize across `call_indirect` from wasm bytecode alone — they see
  an opaque function pointer whose target depends on values they cannot
  constant-fold. Most of what makes Go's JSON slow is also what hides
  specialization opportunities from the JIT.
- **tinygo-regex (kept)** — direct-called scalar code. `regexp.MustCompile`
  builds a DFA, and `FindAllString` is a tight inner loop over a `[]byte`
  doing scalar comparisons and byte loads. Direct calls, regular loops,
  scalar arithmetic — exactly the shape where LLVM `-O3` codegen improves
  on IR JIT `O2` (better instruction scheduling, occasional autovec on a
  memchr-like scan).

This is a useful negative result for the tier-2 story: the patterns that
fight a two-tier wasm JIT — native FFI hops, wasm-compiled interpreters,
and indirect dispatch over runtime type info — are the same patterns that
keep multi-tier JITs in native runtimes (V8, JSC, .NET) from getting linear
speedups on app-style workloads. The kernels where tier-2 already wins big
in the existing suite (`shootout-*`, `blake3`, `gcc-loops`, `rust-*`) have
the opposite shape: direct-called numerical kernels.

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