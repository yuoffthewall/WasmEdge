# Tier-1 / Tier-2 / LLVM JIT WorkTime comparison — Release build

Per-kernel 3-run median **WorkTime (µs)** on `sightglass-strong` (41 measured kernels, including `noop`). `T1/T2` = tier-1 WT / tier-2 WT, `LLVM/T2` = LLVM JIT WT / tier-2 WT — values > 1 mean tier-2 wins. Rows in the tier-2 sections are sorted by **LLVM/T2** descending, with failed rows and `noop` at the bottom.

Build: `CMAKE_BUILD_TYPE=Release`, `WASMEDGE_IR_JIT_OPT_LEVEL=2`, suite `sightglass-strong`. The baseline tier-1 (`WASMEDGE_SIGHTGLASS_MODE=IR_JIT`, tier-2 env unset) and LLVM JIT (`WASMEDGE_SIGHTGLASS_MODE=JIT`) numbers are measured on the `osr` branch (`bdd80e91`); they are reused as the divisors for every `T1/T2` and `LLVM/T2` ratio in §3–§5 so only the tier-2 implementation changes. §3 uses a separate Release build of `promote-hot-callees` (`f07da860`) carrying two backports from `osr`:

- `109386ab fix(ir-jit): save/restore termination jmp_buf across nested invoke` — without this, tier-2 LLVM code calling tier-1 via `jit_host_call` re-enters `IRJitEngine::invoke` and the inner `setjmp` clobbers the outer frame's saved state. A later `longjmp` lands on the popped inner frame and glibc's `__longjmp_chk` aborts with `*** longjmp causes uninitialized stack frame ***`. Reproduced reliably on strengthened `tinygo-regex` once ~30 tier-2 functions are promoted. The osr branch sidesteps the nested-invoke pattern via the entry-thunk path (`76cf55b9`); the focused 20-line save/restore patch fixes promote-hot-callees without dragging that refactor in.
- `f07da860 fix(tier2): replace inline-asm TLS reads with accessor functions` — port of osr commit `df34f9fc`. Tier-2 LLVM-emitted thunks used to read `JitExecEnv*` and `Executor::ExecutionContext` via `movq %fs:OFFSET, $0` inline asm, baking in the initial-exec TLS model. That model breaks when WasmEdge is loaded as a dlopen'd shared library (sightglass-cli's engine plugin path). The fix replaces both inline-asm sites with calls to ORC-bound accessor functions; the entry-thunk-related hunks from df34f9fc were dropped from the port since promote-hot-callees lacks the entry-thunk path.

Excluded: `splay` (no wasm-gc support in IR JIT — segfaults at tier-1). Excluded from geomean aggregates: `noop` (sub-µs work time makes ratios meaningless) and `shootout-ackermann` (high run-to-run variance).

Raw logs:
- Baseline tier-1: `/tmp/wasm-full-20260518-tier1-run{1,2,3}.log`
- LLVM JIT: `/tmp/wasm-full-20260518-llvm-run{1,2,3}.log`
- Promote hot callees: `/tmp/wasm-full-20260518-promote-run{1,2,3}.log`
- Regular tier-2 without OSR: `/tmp/wasm-full-20260518-regular-run{1,2,3}.log`
- Tier-2 with OSR: `/tmp/wasm-full-20260518-osr-run{1,2,3}.log`

Pass counts per phase (out of 41 kernels): tier-1 41/41, LLVM JIT 41/41, promote 41/41, regular tier-2 41/41, OSR 41/41.


## 1. branch `osr` — LLVM JIT baseline

Whole-module LLVM JIT (`WASMEDGE_SIGHTGLASS_MODE=JIT`). 3-run median per kernel. Rows alphabetical (`noop` last). These columns are the divisors for every `LLVM/T2` ratio that appears in §3–§5.

| Kernel | LLVM JIT WT (µs) |
|---|---:|
| blake3-scalar | 5,200,415 |
| blind-sig | 4,156,440 |
| bz2 | 7,060,749 |
| gcc-loops | 6,806,592 |
| hashset | 4,940,947 |
| image-classification | 2,784 |
| pulldown-cmark | 2,179,733 |
| quicksort | 6,805,066 |
| regex | 8,603,822 |
| richards | 732,598 |
| rust-compression | 6,999,568 |
| rust-html-rewriter | 879,841 |
| rust-json | 2,181,605 |
| rust-protobuf | 2,064,158 |
| shootout-ackermann | 3,482,395 |
| shootout-base64 | 6,596,252 |
| shootout-ctype | 4,997,939 |
| shootout-ed25519 | 4,852,592 |
| shootout-fib2 | 5,665,627 |
| shootout-gimli | 8,067,482 |
| shootout-heapsort | 7,952,058 |
| shootout-keccak | 6,859,866 |
| shootout-matrix | 6,771,200 |
| shootout-memmove | 2,645,052 |
| shootout-minicsv | 1,464,896 |
| shootout-nestedloop | 8,886,440 |
| shootout-random | 4,372,971 |
| shootout-ratelimit | 866,915 |
| shootout-seqhash | 5,527,015 |
| shootout-sieve | 7,173,303 |
| shootout-switch | 9,016,662 |
| shootout-xblabla20 | 2,530,955 |
| shootout-xchacha20 | 7,966,930 |
| spidermonkey-json | 65,747 |
| spidermonkey-markdown | 36,193 |
| spidermonkey-regex | 14,900 |
| sqlite3 | 563,305 |
| tinygo-json | 47,576 |
| tinygo-regex | 5,337,782 |
| tract-onnx-image-classification | 169,237 |
| noop | 0 |

### Compilation time and time-to-value

3-run median **compile time (ms)** and **TtV (ms)** for the LLVM JIT path.

| Kernel | Comp LLVM (ms) | TtV LLVM (ms) |
|---|---:|---:|
| blake3-scalar | 687 | 5,882 |
| blind-sig | 3,095 | 7,351 |
| bz2 | 1,102 | 8,168 |
| gcc-loops | 3,723 | 10,566 |
| hashset | 1,991 | 6,903 |
| image-classification | 2,599 | 2,667 |
| pulldown-cmark | 1,915 | 4,099 |
| quicksort | 215 | 7,024 |
| regex | 4,993 | 13,614 |
| richards | 75 | 810 |
| rust-compression | 5,556 | 12,565 |
| rust-html-rewriter | 4,711 | 5,595 |
| rust-json | 1,760 | 3,945 |
| rust-protobuf | 993 | 3,060 |
| shootout-ackermann | 303 | 3,787 |
| shootout-base64 | 281 | 6,879 |
| shootout-ctype | 263 | 5,269 |
| shootout-ed25519 | 2,517 | 7,371 |
| shootout-fib2 | 205 | 5,869 |
| shootout-gimli | 17 | 8,085 |
| shootout-heapsort | 82 | 8,038 |
| shootout-keccak | 517 | 7,378 |
| shootout-matrix | 274 | 7,046 |
| shootout-memmove | 284 | 2,926 |
| shootout-minicsv | 54 | 1,519 |
| shootout-nestedloop | 203 | 9,095 |
| shootout-random | 203 | 4,576 |
| shootout-ratelimit | 276 | 1,143 |
| shootout-seqhash | 311 | 5,846 |
| shootout-sieve | 198 | 7,372 |
| shootout-switch | 954 | 9,980 |
| shootout-xblabla20 | 288 | 2,820 |
| shootout-xchacha20 | 280 | 8,248 |
| spidermonkey-json | 90,669 | 90,880 |
| spidermonkey-markdown | 89,981 | 90,142 |
| spidermonkey-regex | 90,265 | 90,396 |
| sqlite3 | 16,377 | 16,953 |
| tinygo-json | 10,304 | 10,361 |
| tinygo-regex | 7,791 | 13,144 |
| tract-onnx-image-classification | 135,938 | 136,445 |
| noop | 56 | 56 |


## 2. branch `osr` — tier-1 baseline

Tier-1 IR JIT (`WASMEDGE_SIGHTGLASS_MODE=IR_JIT`, tier-2 env unset). 3-run median per kernel. Rows alphabetical (`noop` last). These columns are the divisors for every `T1/T2` ratio that appears in §3–§5, and the Comp T1 / TtV T1 columns are the divisors for every `T2/T1` compile-time and TtV ratio in those sections.

| Kernel | Tier-1 WT (µs) |
|---|---:|
| blake3-scalar | 5,353,822 |
| blind-sig | 9,373,218 |
| bz2 | 8,577,998 |
| gcc-loops | 9,151,021 |
| hashset | 5,804,478 |
| image-classification | 2,199 |
| pulldown-cmark | 3,043,596 |
| quicksort | 8,054,768 |
| regex | 10,114,573 |
| richards | 903,620 |
| rust-compression | 10,567,569 |
| rust-html-rewriter | 1,218,556 |
| rust-json | 3,458,560 |
| rust-protobuf | 2,837,580 |
| shootout-ackermann | 4,694,035 |
| shootout-base64 | 8,433,085 |
| shootout-ctype | 8,788,804 |
| shootout-ed25519 | 8,451,759 |
| shootout-fib2 | 7,886,532 |
| shootout-gimli | 8,088,746 |
| shootout-heapsort | 9,110,507 |
| shootout-keccak | 8,292,107 |
| shootout-matrix | 9,116,232 |
| shootout-memmove | 2,728,614 |
| shootout-minicsv | 2,095,122 |
| shootout-nestedloop | 5,439,761 |
| shootout-random | 6,927,647 |
| shootout-ratelimit | 1,085,281 |
| shootout-seqhash | 5,477,030 |
| shootout-sieve | 8,678,701 |
| shootout-switch | 8,492,810 |
| shootout-xblabla20 | 3,070,611 |
| shootout-xchacha20 | 8,398,324 |
| spidermonkey-json | 81,095 |
| spidermonkey-markdown | 54,853 |
| spidermonkey-regex | 26,367 |
| sqlite3 | 828,592 |
| tinygo-json | 67,877 |
| tinygo-regex | 8,360,584 |
| tract-onnx-image-classification | 270,125 |
| noop | 0 |

### Compilation time and time-to-value

3-run median **compile time (ms)** and **TtV (ms)** for the tier-1 IR JIT path.

| Kernel | Comp T1 (ms) | TtV T1 (ms) |
|---|---:|---:|
| blake3-scalar | 15 | 5,370 |
| blind-sig | 87 | 9,467 |
| bz2 | 34 | 8,615 |
| gcc-loops | 156 | 9,317 |
| hashset | 762 | 6,586 |
| image-classification | 71 | 147 |
| pulldown-cmark | 60 | 3,108 |
| quicksort | 6 | 8,061 |
| regex | 123 | 10,261 |
| richards | 2 | 906 |
| rust-compression | 181 | 10,761 |
| rust-html-rewriter | 118 | 1,348 |
| rust-json | 41 | 3,504 |
| rust-protobuf | 22 | 2,864 |
| shootout-ackermann | 8 | 4,703 |
| shootout-base64 | 8 | 8,442 |
| shootout-ctype | 8 | 8,799 |
| shootout-ed25519 | 147 | 8,588 |
| shootout-fib2 | 6 | 7,893 |
| shootout-gimli | 0 | 8,089 |
| shootout-heapsort | 2 | 9,113 |
| shootout-keccak | 3 | 8,295 |
| shootout-matrix | 8 | 9,125 |
| shootout-memmove | 8 | 2,737 |
| shootout-minicsv | 1 | 2,096 |
| shootout-nestedloop | 6 | 5,446 |
| shootout-random | 6 | 6,934 |
| shootout-ratelimit | 8 | 1,094 |
| shootout-seqhash | 9 | 5,486 |
| shootout-sieve | 6 | 8,686 |
| shootout-switch | 36 | 8,531 |
| shootout-xblabla20 | 8 | 3,079 |
| shootout-xchacha20 | 9 | 8,408 |
| spidermonkey-json | 3,133 | 3,387 |
| spidermonkey-markdown | 3,195 | 3,409 |
| spidermonkey-regex | 3,132 | 3,312 |
| sqlite3 | 340 | 1,189 |
| tinygo-json | 275 | 355 |
| tinygo-regex | 216 | 8,588 |
| tract-onnx-image-classification | 3,565 | 4,251 |
| noop | 1 | 1 |


## 3. branch `promote-hot-callees` — tier-2, Promote Hot Callees

Tier-2 arm from `promote-hot-callees` with `WASMEDGE_TIER2_ENABLE=1`, `WASMEDGE_TIER2_THRESHOLD=10`, `WASMEDGE_TIER2_LOOP_THRESHOLD=5`, and OSR env unset.

| Kernel | Tier-2 | T1/T2 | LLVM/T2 |
|---|---:|---:|---:|
| shootout-nestedloop | 5,440,386 | 1.00× | 1.63× |
| shootout-ackermann | 3,108,238 | 1.51× | 1.12× |
| shootout-switch | 8,595,380 | 0.99× | 1.05× |
| shootout-seqhash | 5,429,584 | 1.01× | 1.02× |
| shootout-fib2 | 5,708,687 | 1.38× | 0.99× |
| shootout-gimli | 8,158,523 | 0.99× | 0.99× |
| shootout-memmove | 2,721,678 | 1.00× | 0.97× |
| shootout-keccak | 7,061,055 | 1.17× | 0.97× |
| quicksort | 7,019,701 | 1.15× | 0.97× |
| blake3-scalar | 5,406,670 | 0.99× | 0.96× |
| shootout-xchacha20 | 8,419,702 | 1.00× | 0.95× |
| image-classification | 2,961 | 0.74× | 0.94× |
| bz2 | 7,590,988 | 1.13× | 0.93× |
| shootout-heapsort | 9,094,983 | 1.00× | 0.87× |
| regex | 10,100,564 | 1.00× | 0.85× |
| shootout-xblabla20 | 3,002,297 | 1.02× | 0.84× |
| tinygo-regex | 6,583,796 | 1.27× | 0.81× |
| shootout-sieve | 8,922,085 | 0.97× | 0.80× |
| spidermonkey-json | 82,917 | 0.98× | 0.79× |
| richards | 927,574 | 0.97× | 0.79× |
| shootout-ratelimit | 1,102,093 | 0.98× | 0.79× |
| shootout-base64 | 8,466,227 | 1.00× | 0.78× |
| rust-compression | 9,292,646 | 1.14× | 0.75× |
| pulldown-cmark | 2,912,271 | 1.05× | 0.75× |
| shootout-matrix | 9,104,066 | 1.00× | 0.74× |
| gcc-loops | 9,193,173 | 1.00× | 0.74× |
| shootout-minicsv | 2,083,213 | 1.01× | 0.70× |
| tinygo-json | 68,337 | 0.99× | 0.70× |
| sqlite3 | 836,743 | 0.99× | 0.67× |
| rust-html-rewriter | 1,359,336 | 0.90× | 0.65× |
| rust-protobuf | 3,231,314 | 0.88× | 0.64× |
| spidermonkey-markdown | 57,066 | 0.96× | 0.63× |
| shootout-random | 6,943,921 | 1.00× | 0.63× |
| tract-onnx-image-classification | 270,004 | 1.00× | 0.63× |
| hashset | 7,916,773 | 0.73× | 0.62× |
| rust-json | 3,545,115 | 0.98× | 0.62× |
| spidermonkey-regex | 27,119 | 0.97× | 0.55× |
| shootout-ed25519 | 12,846,623 | 0.66× | 0.38× |
| blind-sig | 11,386,847 | 0.82× | 0.37× |
| shootout-ctype | 16,316,727 | 0.54× | 0.31× |
| noop | 0 | — | — |

Aggregates (39 kernels, `noop`, `shootout-ackermann`, and any failed rows excluded): geomean T1/T2 **0.972×**, geomean LLVM/T2 **0.756×**.

### Compilation time and time-to-value

3-run median **compile time (ms)** and **TtV (ms)**. Compile-time ratios use the same direction as the previous note: `T2/T1` and `LLVM/T2`.

| Kernel | Comp T2 (ms) | LLVM/T2 Comp | TtV T2 (ms) | LLVM/T2 TtV |
|---|---:|---:|---:|---:|
| blake3-scalar | 16 | 42.10× | 5,425 | 1.08× |
| blind-sig | 88 | 35.05× | 11,483 | 0.64× |
| bz2 | 35 | 31.06× | 7,629 | 1.07× |
| gcc-loops | 167 | 22.25× | 9,372 | 1.13× |
| hashset | 719 | 2.77× | 8,649 | 0.80× |
| image-classification | 75 | 34.77× | 152 | 17.52× |
| pulldown-cmark | 61 | 31.45× | 2,978 | 1.38× |
| quicksort | 6 | 34.90× | 7,026 | 1.00× |
| regex | 129 | 38.74× | 10,260 | 1.33× |
| richards | 2 | 34.39× | 930 | 0.87× |
| rust-compression | 186 | 29.79× | 9,499 | 1.32× |
| rust-html-rewriter | 126 | 37.47× | 1,495 | 3.74× |
| rust-json | 44 | 40.09× | 3,594 | 1.10× |
| rust-protobuf | 23 | 42.51× | 3,258 | 0.94× |
| shootout-ackermann | 9 | 34.76× | 3,118 | 1.21× |
| shootout-base64 | 8 | 34.50× | 8,475 | 0.81× |
| shootout-ctype | 8 | 32.37× | 16,330 | 0.32× |
| shootout-ed25519 | 122 | 20.60× | 12,974 | 0.57× |
| shootout-fib2 | 6 | 33.72× | 5,715 | 1.03× |
| shootout-gimli | 0 | 44.01× | 8,159 | 0.99× |
| shootout-heapsort | 2 | 34.54× | 9,098 | 0.88× |
| shootout-keccak | 3 | 165.88× | 7,065 | 1.04× |
| shootout-matrix | 8 | 33.94× | 9,113 | 0.77× |
| shootout-memmove | 8 | 35.36× | 2,734 | 1.07× |
| shootout-minicsv | 1 | 50.47× | 2,084 | 0.73× |
| shootout-nestedloop | 7 | 31.21× | 5,448 | 1.67× |
| shootout-random | 7 | 29.43× | 6,950 | 0.66× |
| shootout-ratelimit | 8 | 33.32× | 1,111 | 1.03× |
| shootout-seqhash | 9 | 35.44× | 5,439 | 1.07× |
| shootout-sieve | 6 | 32.38× | 8,930 | 0.83× |
| shootout-switch | 36 | 26.20× | 8,640 | 1.16× |
| shootout-xblabla20 | 8 | 35.35× | 3,011 | 0.94× |
| shootout-xchacha20 | 9 | 32.14× | 8,431 | 0.98× |
| spidermonkey-json | 2,381 | 38.08× | 2,645 | 34.36× |
| spidermonkey-markdown | 2,397 | 37.54× | 2,613 | 34.50× |
| spidermonkey-regex | 2,396 | 37.68× | 2,578 | 35.07× |
| sqlite3 | 363 | 45.11× | 1,217 | 13.93× |
| tinygo-json | 281 | 36.61× | 361 | 28.70× |
| tinygo-regex | 224 | 34.80× | 6,832 | 1.92× |
| tract-onnx-image-classification | 3,663 | 37.11× | 4,371 | 31.22× |
| noop | 1 | — | 1 | — |

Aggregates (39 kernels, `noop`, `shootout-ackermann`, and any failed rows excluded): geomean Comp T2/T1 **1.009×**, Comp LLVM/T2 **33.724×**, TtV T2/T1 **1.001×**, TtV LLVM/T2 **1.797×**.


## 4. branch `osr` — regular tier-2 (Walk up + BFS, no OSR)

Tier-2 arm from `osr` with `WASMEDGE_TIER2_ENABLE=1`, `WASMEDGE_TIER2_THRESHOLD=10`, and `WASMEDGE_OSR_THRESHOLD=0`.

| Kernel | Tier-2 | T1/T2 | LLVM/T2 |
|---|---:|---:|---:|
| shootout-nestedloop | 5,431,988 | 1.00× | 1.64× |
| shootout-ackermann | 2,774,818 | 1.69× | 1.25× |
| image-classification | 2,248 | 0.98× | 1.24× |
| shootout-switch | 8,614,484 | 0.99× | 1.05× |
| shootout-seqhash | 5,417,119 | 1.01× | 1.02× |
| blake3-scalar | 5,233,309 | 1.02× | 0.99× |
| shootout-gimli | 8,153,746 | 0.99× | 0.99× |
| shootout-keccak | 6,971,609 | 1.19× | 0.98× |
| bz2 | 7,187,289 | 1.19× | 0.98× |
| shootout-memmove | 2,707,690 | 1.01× | 0.98× |
| quicksort | 6,996,109 | 1.15× | 0.97× |
| regex | 9,014,088 | 1.12× | 0.95× |
| shootout-xchacha20 | 8,434,705 | 1.00× | 0.94× |
| shootout-heapsort | 9,061,037 | 1.01× | 0.88× |
| tinygo-regex | 6,086,568 | 1.37× | 0.88× |
| rust-compression | 8,044,600 | 1.31× | 0.87× |
| shootout-fib2 | 6,515,013 | 1.21× | 0.87× |
| pulldown-cmark | 2,584,154 | 1.18× | 0.84× |
| shootout-xblabla20 | 3,007,163 | 1.02× | 0.84× |
| hashset | 5,894,940 | 0.98× | 0.84× |
| gcc-loops | 8,201,099 | 1.12× | 0.83× |
| richards | 908,661 | 0.99× | 0.81× |
| shootout-sieve | 8,902,376 | 0.97× | 0.81× |
| spidermonkey-json | 82,294 | 0.99× | 0.80× |
| shootout-ratelimit | 1,092,851 | 0.99× | 0.79× |
| shootout-minicsv | 1,848,205 | 1.13× | 0.79× |
| shootout-base64 | 8,429,324 | 1.00× | 0.78× |
| shootout-matrix | 9,028,701 | 1.01× | 0.75× |
| rust-json | 2,925,572 | 1.18× | 0.75× |
| rust-protobuf | 2,894,284 | 0.98× | 0.71× |
| tinygo-json | 68,220 | 0.99× | 0.70× |
| sqlite3 | 829,596 | 1.00× | 0.68× |
| rust-html-rewriter | 1,313,456 | 0.93× | 0.67× |
| spidermonkey-markdown | 55,658 | 0.99× | 0.65× |
| tract-onnx-image-classification | 267,646 | 1.01× | 0.63× |
| shootout-random | 6,940,275 | 1.00× | 0.63× |
| shootout-ed25519 | 8,424,722 | 1.00× | 0.58× |
| spidermonkey-regex | 27,206 | 0.97× | 0.55× |
| blind-sig | 10,517,630 | 0.89× | 0.40× |
| shootout-ctype | 13,331,031 | 0.66× | 0.37× |
| noop | 0 | — | — |

Aggregates (39 kernels, `noop`, `shootout-ackermann`, and any failed rows excluded): geomean T1/T2 **1.033×**, geomean LLVM/T2 **0.804×**.

### Compilation time and time-to-value

3-run median **compile time (ms)** and **TtV (ms)**. Compile-time ratios use the same direction as the previous note: `T2/T1` and `LLVM/T2`.

| Kernel | Comp T2 (ms) | LLVM/T2 Comp | TtV T2 (ms) | LLVM/T2 TtV |
|---|---:|---:|---:|---:|
| blake3-scalar | 94 | 7.31× | 5,330 | 1.10× |
| blind-sig | 236 | 13.13× | 10,761 | 0.68× |
| bz2 | 63 | 17.60× | 7,254 | 1.13× |
| gcc-loops | 618 | 6.03× | 8,832 | 1.20× |
| hashset | 844 | 2.36× | 6,735 | 1.02× |
| image-classification | 228 | 11.38× | 301 | 8.86× |
| pulldown-cmark | 178 | 10.76× | 2,765 | 1.48× |
| quicksort | 21 | 10.46× | 7,017 | 1.00× |
| regex | 375 | 13.32× | 9,428 | 1.44× |
| richards | 7 | 10.17× | 916 | 0.88× |
| rust-compression | 369 | 15.06× | 8,424 | 1.49× |
| rust-html-rewriter | 392 | 12.02× | 1,718 | 3.26× |
| rust-json | 164 | 10.70× | 3,096 | 1.27× |
| rust-protobuf | 121 | 8.20× | 3,020 | 1.01× |
| shootout-ackermann | 30 | 10.26× | 2,805 | 1.35× |
| shootout-base64 | 25 | 11.27× | 8,455 | 0.81× |
| shootout-ctype | 24 | 11.10× | 13,355 | 0.39× |
| shootout-ed25519 | 124 | 20.22× | 8,550 | 0.86× |
| shootout-fib2 | 20 | 10.33× | 6,539 | 0.90× |
| shootout-gimli | 4 | 4.27× | 8,159 | 0.99× |
| shootout-heapsort | 8 | 10.83× | 9,069 | 0.89× |
| shootout-keccak | 7 | 76.46× | 6,980 | 1.06× |
| shootout-matrix | 24 | 11.44× | 9,053 | 0.78× |
| shootout-memmove | 23 | 12.58× | 2,731 | 1.07× |
| shootout-minicsv | 6 | 8.85× | 1,855 | 0.82× |
| shootout-nestedloop | 20 | 9.95× | 5,453 | 1.67× |
| shootout-random | 21 | 9.50× | 6,961 | 0.66× |
| shootout-ratelimit | 24 | 11.59× | 1,117 | 1.02× |
| shootout-seqhash | 23 | 13.37× | 5,442 | 1.07× |
| shootout-sieve | 20 | 9.74× | 8,923 | 0.83× |
| shootout-switch | 58 | 16.33× | 8,692 | 1.15× |
| shootout-xblabla20 | 26 | 10.95× | 3,034 | 0.93× |
| shootout-xchacha20 | 22 | 12.57× | 8,458 | 0.98× |
| spidermonkey-json | 6,308 | 14.37× | 6,577 | 13.82× |
| spidermonkey-markdown | 6,326 | 14.22× | 6,551 | 13.76× |
| spidermonkey-regex | 6,314 | 14.30× | 6,504 | 13.90× |
| sqlite3 | 1,064 | 15.39× | 1,913 | 8.86× |
| tinygo-json | 383 | 26.90× | 463 | 22.37× |
| tinygo-regex | 295 | 26.40× | 6,396 | 2.05× |
| tract-onnx-image-classification | 8,279 | 16.42× | 8,990 | 15.18× |
| noop | 10 | — | 10 | — |

Aggregates (39 kernels, `noop`, `shootout-ackermann`, and any failed rows excluded): geomean Comp T2/T1 **2.831×**, Comp LLVM/T2 **12.021×**, TtV T2/T1 **1.091×**, TtV LLVM/T2 **1.648×**.


## 5. branch `osr` — tier-2 + OSR (Walk up + BFS + OSR)

Tier-2 arm from `osr` with `WASMEDGE_TIER2_ENABLE=1`, `WASMEDGE_TIER2_THRESHOLD=10`, and `WASMEDGE_OSR_THRESHOLD=5000`.

| Kernel | Tier-2 | T1/T2 | LLVM/T2 |
|---|---:|---:|---:|
| shootout-nestedloop | 5,456,438 | 1.00× | 1.63× |
| image-classification | 2,182 | 1.01× | 1.28× |
| shootout-ackermann | 2,752,725 | 1.71× | 1.27× |
| quicksort | 6,544,738 | 1.23× | 1.04× |
| shootout-gimli | 7,929,693 | 1.02× | 1.02× |
| shootout-seqhash | 5,435,950 | 1.01× | 1.02× |
| shootout-switch | 8,998,447 | 0.94× | 1.00× |
| shootout-xchacha20 | 7,969,919 | 1.05× | 1.00× |
| shootout-random | 4,383,751 | 1.58× | 1.00× |
| blake3-scalar | 5,256,128 | 1.02× | 0.99× |
| shootout-keccak | 6,935,148 | 1.20× | 0.99× |
| shootout-base64 | 6,676,434 | 1.26× | 0.99× |
| shootout-memmove | 2,695,377 | 1.01× | 0.98× |
| shootout-heapsort | 8,213,369 | 1.11× | 0.97× |
| bz2 | 7,298,940 | 1.18× | 0.97× |
| shootout-ratelimit | 901,536 | 1.20× | 0.96× |
| shootout-minicsv | 1,535,510 | 1.36× | 0.95× |
| shootout-xblabla20 | 2,664,736 | 1.15× | 0.95× |
| shootout-matrix | 7,140,199 | 1.28× | 0.95× |
| regex | 9,085,284 | 1.11× | 0.95× |
| shootout-ctype | 5,664,109 | 1.55× | 0.88× |
| tinygo-regex | 6,092,834 | 1.37× | 0.88× |
| shootout-fib2 | 6,506,106 | 1.21× | 0.87× |
| gcc-loops | 7,916,342 | 1.16× | 0.86× |
| rust-compression | 8,467,747 | 1.25× | 0.83× |
| shootout-sieve | 8,730,913 | 0.99× | 0.82× |
| hashset | 6,176,044 | 0.94× | 0.80× |
| blind-sig | 5,201,661 | 1.80× | 0.80× |
| richards | 934,762 | 0.97× | 0.78× |
| pulldown-cmark | 2,801,286 | 1.09× | 0.78× |
| spidermonkey-json | 93,950 | 0.86× | 0.70× |
| rust-protobuf | 2,951,811 | 0.96× | 0.70× |
| tinygo-json | 69,082 | 0.98× | 0.69× |
| shootout-ed25519 | 7,318,327 | 1.15× | 0.66× |
| sqlite3 | 867,947 | 0.95× | 0.65× |
| rust-json | 3,444,532 | 1.00× | 0.63× |
| tract-onnx-image-classification | 271,405 | 1.00× | 0.62× |
| rust-html-rewriter | 1,417,953 | 0.86× | 0.62× |
| spidermonkey-markdown | 58,657 | 0.94× | 0.62× |
| spidermonkey-regex | 27,769 | 0.95× | 0.54× |
| noop | 0 | — | — |

Aggregates (39 kernels, `noop`, `shootout-ackermann`, and any failed rows excluded): geomean T1/T2 **1.105×**, geomean LLVM/T2 **0.860×**.

### Compilation time and time-to-value

3-run median **compile time (ms)** and **TtV (ms)**. Compile-time ratios use the same direction as the previous note: `T2/T1` and `LLVM/T2`.

| Kernel | Comp T2 (ms) | LLVM/T2 Comp | TtV T2 (ms) | LLVM/T2 TtV |
|---|---:|---:|---:|---:|
| blake3-scalar | 95 | 7.21× | 5,353 | 1.10× |
| blind-sig | 259 | 11.94× | 5,462 | 1.35× |
| bz2 | 82 | 13.39× | 7,384 | 1.11× |
| gcc-loops | 645 | 5.77× | 8,587 | 1.23× |
| hashset | 419 | 4.75× | 6,623 | 1.04× |
| image-classification | 235 | 11.08× | 303 | 8.80× |
| pulldown-cmark | 184 | 10.39× | 2,992 | 1.37× |
| quicksort | 23 | 9.55× | 6,568 | 1.07× |
| regex | 401 | 12.46× | 9,507 | 1.43× |
| richards | 8 | 8.94× | 943 | 0.86× |
| rust-compression | 415 | 13.38× | 8,896 | 1.41× |
| rust-html-rewriter | 427 | 11.04× | 1,857 | 3.01× |
| rust-json | 175 | 10.04× | 3,626 | 1.09× |
| rust-protobuf | 126 | 7.86× | 3,083 | 0.99× |
| shootout-ackermann | 33 | 9.28× | 2,786 | 1.36× |
| shootout-base64 | 29 | 9.84× | 6,704 | 1.03× |
| shootout-ctype | 27 | 9.69× | 5,692 | 0.93× |
| shootout-ed25519 | 136 | 18.51× | 7,456 | 0.99× |
| shootout-fib2 | 22 | 9.25× | 6,529 | 0.90× |
| shootout-gimli | 4 | 4.39× | 7,934 | 1.02× |
| shootout-heapsort | 8 | 9.69× | 8,222 | 0.98× |
| shootout-keccak | 7 | 76.02× | 6,942 | 1.06× |
| shootout-matrix | 27 | 10.02× | 7,168 | 0.98× |
| shootout-memmove | 26 | 11.15× | 2,722 | 1.08× |
| shootout-minicsv | 6 | 8.89× | 1,542 | 0.99× |
| shootout-nestedloop | 23 | 8.78× | 5,482 | 1.66× |
| shootout-random | 22 | 9.30× | 4,406 | 1.04× |
| shootout-ratelimit | 28 | 9.95× | 930 | 1.23× |
| shootout-seqhash | 28 | 11.22× | 5,464 | 1.07× |
| shootout-sieve | 21 | 9.27× | 8,753 | 0.84× |
| shootout-switch | 55 | 17.33× | 9,076 | 1.10× |
| shootout-xblabla20 | 27 | 10.59× | 2,691 | 1.05× |
| shootout-xchacha20 | 26 | 10.60× | 7,997 | 1.03× |
| spidermonkey-json | 8,693 | 10.43× | 8,976 | 10.13× |
| spidermonkey-markdown | 8,695 | 10.35× | 8,920 | 10.11× |
| spidermonkey-regex | 8,721 | 10.35× | 8,910 | 10.15× |
| sqlite3 | 1,099 | 14.90× | 1,985 | 8.54× |
| tinygo-json | 392 | 26.29× | 473 | 21.93× |
| tinygo-regex | 293 | 26.58× | 6,402 | 2.05× |
| tract-onnx-image-classification | 8,932 | 15.22× | 9,657 | 14.13× |
| noop | 12 | — | 12 | — |

Aggregates (39 kernels, `noop`, `shootout-ackermann`, and any failed rows excluded): geomean Comp T2/T1 **3.053×**, Comp LLVM/T2 **11.145×**, TtV T2/T1 **1.041×**, TtV LLVM/T2 **1.728×**.

---

Note: `noop` is retained as a sanity row, but excluded from geomean aggregates because the sub-microsecond work time makes ratios meaningless. `shootout-ackermann` is excluded from geomean aggregates because of high run-to-run variance (bimodal across 3 runs). `splay` is not measured because IR JIT does not yet support the wasm-gc instructions the splay.wat kernel uses; it segfaults at tier-1.
