# Tier-1 / Tier-2 / LLVM JIT WorkTime comparison — Release build

Per-kernel 3-run median **WorkTime (µs)** on `sightglass-strong` (33
kernels). `t1/t2` = tier-1 WT / tier-2 WT, `LLVM/t2` = LLVM JIT WT /
tier-2 WT — values > 1 mean tier-2 wins. Rows sorted by **LLVM/t2**
descending.

Build: `CMAKE_BUILD_TYPE=Release`, `WASMEDGE_IR_JIT_OPT_LEVEL=2`,
suite `sightglass-strong`. Tier-1 (`TIER2_ENABLE=0`) and LLVM JIT
(`MODE=JIT`) numbers are reused across §2–§4 from a single 2dc9ef7b
run; only the tier-2 column changes per section. The LLVM JIT
baseline below is the divisor for every `LLVM/t2` ratio in §2–§4.

Logs at `/tmp/wasm-<commit>-{tier1,tier2,llvm}-run{1,2,3}.log` and
`/tmp/wasm-2dc9ef7b-tier2only-run{1,2,3}.log` (§3).

## 1. 2dc9ef7b — LLVM JIT baseline

Whole-module LLVM JIT (`WASMEDGE_SIGHTGLASS_MODE=JIT`). 3-run
median per kernel. Rows alphabetical (`noop` last). These columns
are the divisors for every `LLVM/t2` ratio that appears in §2–§4.

| Kernel | LLVM JIT WT (µs) |
|---|---:|
| blake3-scalar | 5,214,484 |
| blind-sig | 3,638,597 |
| bz2 | 6,995,518 |
| gcc-loops | 6,608,757 |
| hashset | 5,346,923 |
| pulldown-cmark | 2,186,000 |
| quicksort | 7,024,610 |
| regex | 8,582,285 |
| richards | 745,960 |
| rust-compression | 6,999,674 |
| rust-html-rewriter | 855,166 |
| rust-json | 2,167,699 |
| rust-protobuf | 2,046,396 |
| shootout-ackermann | 8,053,812 |
| shootout-base64 | 6,758,004 |
| shootout-ctype | 4,994,419 |
| shootout-ed25519 | 4,995,236 |
| shootout-fib2 | 5,726,646 |
| shootout-gimli | 8,001,214 |
| shootout-heapsort | 8,336,287 |
| shootout-keccak | 6,926,285 |
| shootout-matrix | 6,906,804 |
| shootout-memmove | 2,635,747 |
| shootout-minicsv | 1,417,238 |
| shootout-nestedloop | 8,867,429 |
| shootout-random | 4,390,723 |
| shootout-ratelimit | 882,984 |
| shootout-seqhash | 5,721,465 |
| shootout-sieve | 7,171,015 |
| shootout-switch | 9,069,258 |
| shootout-xblabla20 | 2,691,703 |
| shootout-xchacha20 | 7,735,863 |
| noop | 0 |

### Compilation time and time-to-value

3-run median **`Inst.Lat` (compile/instantiation, ms)** and **`TtV`
(end-to-end wall-clock, ms)** for the LLVM JIT path.

| Kernel | Inst.Lat LLVM (ms) | TtV LLVM (ms) |
|---|---:|---:|
| blake3-scalar | 750 | 5,980 |
| blind-sig | 3,346 | 7,034 |
| bz2 | 1,156 | 8,108 |
| gcc-loops | 4,224 | 10,895 |
| hashset | 2,390 | 7,805 |
| pulldown-cmark | 2,108 | 4,296 |
| quicksort | 215 | 7,169 |
| regex | 5,408 | 14,082 |
| richards | 79 | 823 |
| rust-compression | 6,064 | 13,038 |
| rust-html-rewriter | 5,104 | 5,978 |
| rust-json | 1,944 | 4,136 |
| rust-protobuf | 1,029 | 3,097 |
| shootout-ackermann | 307 | 4,961 |
| shootout-base64 | 293 | 7,038 |
| shootout-ctype | 273 | 5,247 |
| shootout-ed25519 | 2,454 | 7,427 |
| shootout-fib2 | 211 | 5,916 |
| shootout-gimli | 14 | 8,004 |
| shootout-heapsort | 87 | 8,478 |
| shootout-keccak | 368 | 7,275 |
| shootout-matrix | 289 | 7,113 |
| shootout-memmove | 294 | 2,960 |
| shootout-minicsv | 51 | 1,482 |
| shootout-nestedloop | 214 | 9,102 |
| shootout-random | 206 | 4,649 |
| shootout-ratelimit | 291 | 1,167 |
| shootout-seqhash | 323 | 6,045 |
| shootout-sieve | 229 | 7,535 |
| shootout-switch | 2,735 | 11,785 |
| shootout-xblabla20 | 295 | 2,989 |
| shootout-xchacha20 | 293 | 8,103 |
| noop | 55 | 56 |

## 2. 89e83770 — tier-2 - Promote Hot Callees

Envs: `TIER2_ENABLE=1`, `TIER2_THRESHOLD=10`, `TIER2_LOOP_THRESHOLD=5`.
Tier-2 column comes from the original 89e83770 measurement. The
tier-1 column and the `LLVM/t2` ratios use the current-head
(2dc9ef7b) tier-1 and §1 LLVM JIT baselines, so this section
compares 89e83770's tier-2 against today's tier-1 / LLVM JIT.

| Kernel | Tier-1 | Tier-2 | t1/t2 | LLVM/t2 |
|---|---:|---:|---:|---:|
| shootout-nestedloop | 5,437,620 | 5,464,670 | 1.00× | 1.62× |
| shootout-ackermann† | 8,421,700 | 6,123,510 | 1.38× | 1.32×† |
| shootout-seqhash | 5,470,318 | 5,454,939 | 1.00× | 1.05× |
| shootout-switch | 8,654,780 | 8,714,749 | 0.99× | 1.04× |
| quicksort | 8,131,002 | 6,962,820 | 1.17× | 1.01× |
| shootout-keccak | 8,229,748 | 6,909,272 | 1.19× | 1.00× |
| shootout-fib2 | 7,899,479 | 5,733,709 | 1.38× | 1.00× |
| shootout-gimli | 8,177,779 | 8,071,569 | 1.01× | 0.99× |
| shootout-heapsort | 8,438,098 | 8,557,783 | 0.99× | 0.97× |
| shootout-memmove | 2,772,908 | 2,707,938 | 1.02× | 0.97× |
| blake3-scalar | 5,349,408 | 5,376,025 | 1.00× | 0.97× |
| shootout-xblabla20 | 2,877,095 | 2,853,115 | 1.01× | 0.94× |
| regex | 9,212,065 | 9,207,907 | 1.00× | 0.93× |
| bz2 | 7,879,578 | 7,511,928 | 1.05× | 0.93× |
| shootout-xchacha20 | 8,369,227 | 8,352,444 | 1.00× | 0.93× |
| shootout-sieve | 8,138,509 | 8,214,942 | 0.99× | 0.87× |
| gcc-loops | 7,706,566 | 7,718,141 | 1.00× | 0.86× |
| shootout-matrix | 8,158,768 | 8,145,261 | 1.00× | 0.85× |
| shootout-base64 | 8,041,983 | 8,023,518 | 1.00× | 0.84× |
| richards | 914,942 | 905,745 | 1.01× | 0.82× |
| shootout-ratelimit | 1,097,283 | 1,086,098 | 1.01× | 0.81× |
| rust-compression | 9,966,981 | 8,968,602 | 1.11× | 0.78× |
| pulldown-cmark | 2,913,632 | 2,877,061 | 1.01× | 0.76× |
| hashset | 5,692,391 | 7,939,384 | 0.72× | 0.67× |
| shootout-minicsv | 2,008,480 | 2,119,326 | 0.95× | 0.67× |
| rust-html-rewriter | 1,269,313 | 1,308,031 | 0.97× | 0.65× |
| rust-protobuf | 2,779,775 | 3,132,637 | 0.89× | 0.65× |
| rust-json | 3,286,851 | 3,432,358 | 0.96× | 0.63× |
| shootout-random | 6,955,889 | 6,967,795 | 1.00× | 0.63× |
| shootout-ed25519 | 8,286,597 | 12,940,674 | 0.64× | 0.39× |
| blind-sig | 9,809,862 | 10,993,774 | 0.89× | 0.33× |
| shootout-ctype | 8,279,160 | 16,305,276 | 0.51× | 0.31× |
| noop | 0 | 0 | — | — |

Aggregates (32 kernels, `noop` excluded): geomean t1/t2 **0.980×**, geomean LLVM/t2 **0.807×**.

### Compilation time and time-to-value

Same kernel sort order as the WT table. Inst.Lat is the upfront cost
paid before the first wasm instruction runs; TtV is the end-to-end
wall-clock (`Inst.Lat + WT`).

| Kernel | Inst.Lat T1 (ms) | Inst.Lat T2 (ms) | LLVM/T2 Inst | TtV T1 (ms) | TtV T2 (ms) | LLVM/T2 TtV |
|---|---:|---:|---:|---:|---:|---:|
| shootout-nestedloop | 6.1 | 6.2 | 34.52× | 5,460 | 5,474 | 1.66× |
| shootout-ackermann | 8.3 | 8.5 | 36.12× | 6,718 | 6,134 | 0.81× |
| shootout-seqhash | 8.2 | 8.5 | 38.00× | 5,491 | 5,464 | 1.11× |
| shootout-switch | 52 | 50 | 54.70× | 8,802 | 8,770 | 1.34× |
| quicksort | 6.1 | 6.3 | 34.13× | 8,174 | 6,970 | 1.03× |
| shootout-keccak | 2.6 | 2.7 | 136.30× | 8,291 | 6,913 | 1.05× |
| shootout-fib2 | 6.0 | 6.1 | 34.59× | 7,965 | 5,740 | 1.03× |
| shootout-gimli | 0.35 | 0.39 | 35.90× | 8,193 | 8,072 | 0.99× |
| shootout-heapsort | 2.1 | 2.2 | 39.55× | 8,517 | 8,560 | 0.99× |
| shootout-memmove | 7.6 | 8.1 | 36.30× | 2,742 | 2,717 | 1.09× |
| blake3-scalar | 15 | 17 | 44.12× | 5,353 | 5,399 | 1.11× |
| shootout-xblabla20 | 7.7 | 8.3 | 35.54× | 2,998 | 2,862 | 1.04× |
| regex | 125 | 128 | 42.25× | 9,460 | 9,353 | 1.51× |
| bz2 | 36 | 33 | 35.03× | 7,964 | 7,548 | 1.07× |
| shootout-xchacha20 | 7.9 | 8.2 | 35.73× | 8,353 | 8,362 | 0.97× |
| shootout-sieve | 5.9 | 6.0 | 38.17× | 8,145 | 8,221 | 0.92× |
| gcc-loops | 158 | 163 | 25.91× | 7,877 | 7,888 | 1.38× |
| shootout-matrix | 7.7 | 7.8 | 37.05× | 8,149 | 8,154 | 0.87× |
| shootout-base64 | 7.8 | 8.0 | 36.62× | 8,049 | 8,032 | 0.88× |
| richards | 1.9 | 2.0 | 39.50× | 913 | 908 | 0.91× |
| shootout-ratelimit | 7.5 | 7.9 | 36.84× | 1,117 | 1,095 | 1.07× |
| rust-compression | 181 | 188 | 32.26× | 10,158 | 9,174 | 1.42× |
| pulldown-cmark | 59 | 61 | 34.56× | 2,994 | 2,943 | 1.46× |
| hashset | 857 | 825 | 2.90× | 6,580 | 8,771 | 0.89× |
| shootout-minicsv | 1.1 | 1.1 | 46.36× | 2,007 | 2,121 | 0.70× |
| rust-html-rewriter | 117 | 122 | 41.84× | 1,376 | 1,449 | 4.13× |
| rust-protobuf | 22 | 24 | 42.88× | 2,767 | 3,159 | 0.98× |
| rust-json | 41 | 43 | 45.21× | 3,429 | 3,479 | 1.19× |
| shootout-random | 5.8 | 6.0 | 34.33× | 6,971 | 6,974 | 0.67× |
| shootout-ed25519 | 120 | 123 | 19.95× | 8,381 | 13,071 | 0.57× |
| blind-sig | 86 | 88 | 38.02× | 10,000 | 11,088 | 0.63× |
| shootout-ctype | 7.6 | 8.0 | 34.12× | 8,327 | 16,314 | 0.32× |
| noop | 1.1 | 1.2 | 45.83× | 1 | 1 | 56.00× |

Aggregates (32 kernels, `noop` excluded): geomean Inst.Lat T2/T1 **1.03×**, Inst.Lat LLVM/T2 **35.4×**, TtV T2/T1 **1.02×**, TtV LLVM/T2 **1.02×**.

## 3. 2dc9ef7b — Regular tier-2 (Walk up + BFS, no OSR)

Envs: `TIER2_ENABLE=1`, `TIER2_THRESHOLD=10`, `OSR_THRESHOLD=0` (OSR
disabled). Walk-up + BFS callee-promotion at the current head, with
the OSR back-edge instrumentation off. Tier-2 numbers are 3-run
medians from `/tmp/wasm-2dc9ef7b-tier2only-run{1,2,3}.log`.

| Kernel | Tier-1 | Tier-2 | t1/t2 | LLVM/t2 |
|---|---:|---:|---:|---:|
| shootout-ackermann† | 8,421,700 | 2,949,633 | 2.86× | 2.73×† |
| shootout-nestedloop | 5,437,620 | 5,434,859 | 1.00× | 1.63× |
| shootout-seqhash | 5,470,318 | 5,432,607 | 1.01× | 1.05× |
| shootout-switch | 8,654,780 | 8,728,802 | 0.99× | 1.04× |
| quicksort | 8,131,002 | 6,953,815 | 1.17× | 1.01× |
| shootout-keccak | 8,229,748 | 6,926,765 | 1.19× | 1.00× |
| blake3-scalar | 5,349,408 | 5,257,754 | 1.02× | 0.99× |
| shootout-gimli | 8,177,779 | 8,122,575 | 1.01× | 0.99× |
| regex | 9,212,065 | 8,733,690 | 1.05× | 0.98× |
| bz2 | 7,879,578 | 7,141,153 | 1.10× | 0.98× |
| shootout-memmove | 2,772,908 | 2,692,998 | 1.03× | 0.98× |
| shootout-heapsort | 8,438,098 | 8,587,464 | 0.98× | 0.97× |
| shootout-xblabla20 | 2,877,095 | 2,798,153 | 1.03× | 0.96× |
| hashset | 5,692,391 | 5,644,482 | 1.01× | 0.95× |
| shootout-xchacha20 | 8,369,227 | 8,250,137 | 1.01× | 0.94× |
| rust-compression | 9,966,981 | 7,830,809 | 1.27× | 0.89× |
| pulldown-cmark | 2,913,632 | 2,471,188 | 1.18× | 0.88× |
| gcc-loops | 7,706,566 | 7,559,618 | 1.02× | 0.87× |
| shootout-fib2 | 7,899,479 | 6,559,578 | 1.20× | 0.87× |
| shootout-sieve | 8,138,509 | 8,222,018 | 0.99× | 0.87× |
| shootout-matrix | 8,158,768 | 8,168,049 | 1.00× | 0.85× |
| shootout-base64 | 8,041,983 | 8,002,153 | 1.00× | 0.84× |
| rust-protobuf | 2,779,775 | 2,509,769 | 1.11× | 0.82× |
| richards | 914,942 | 915,390 | 1.00× | 0.81× |
| shootout-minicsv | 2,008,480 | 1,755,141 | 1.14× | 0.81× |
| shootout-ratelimit | 1,097,283 | 1,095,792 | 1.00× | 0.81× |
| rust-json | 3,286,851 | 2,737,577 | 1.20× | 0.79× |
| rust-html-rewriter | 1,269,313 | 1,204,253 | 1.05× | 0.71× |
| shootout-ed25519 | 8,286,597 | 7,198,099 | 1.15× | 0.69× |
| shootout-ctype | 8,279,160 | 7,804,607 | 1.06× | 0.64× |
| shootout-random | 6,955,889 | 6,966,852 | 1.00× | 0.63× |
| blind-sig | 9,809,862 | 7,329,701 | 1.34× | 0.50× |
| noop | 0 | 0 | — | — |

Aggregates (32 kernels, `noop` excluded): geomean t1/t2 **1.105×**, geomean LLVM/t2 **0.909×**.

### Compilation time and time-to-value

Same kernel sort order as the WT table.

| Kernel | Inst.Lat T1 (ms) | Inst.Lat T2 (ms) | LLVM/T2 Inst | TtV T1 (ms) | TtV T2 (ms) | LLVM/T2 TtV |
|---|---:|---:|---:|---:|---:|---:|
| shootout-ackermann | 8.3 | 28 | 10.90× | 6,718 | 2,980 | 1.66× |
| shootout-nestedloop | 6.1 | 19 | 11.26× | 5,460 | 5,454 | 1.67× |
| shootout-seqhash | 8.2 | 22 | 14.58× | 5,491 | 5,455 | 1.11× |
| shootout-switch | 52 | 67 | 40.77× | 8,802 | 8,797 | 1.34× |
| quicksort | 6.1 | 20 | 10.89× | 8,174 | 6,974 | 1.03× |
| shootout-keccak | 2.6 | 6.8 | 53.80× | 8,291 | 6,934 | 1.05× |
| blake3-scalar | 15 | 89 | 8.40× | 5,353 | 5,354 | 1.12× |
| shootout-gimli | 0.35 | 4.3 | 3.28× | 8,193 | 8,127 | 0.98× |
| regex | 125 | 373 | 14.48× | 9,460 | 9,125 | 1.54× |
| bz2 | 36 | 63 | 18.49× | 7,964 | 7,209 | 1.12× |
| shootout-memmove | 7.6 | 22 | 13.16× | 2,742 | 2,716 | 1.09× |
| shootout-heapsort | 2.1 | 8.0 | 10.85× | 8,517 | 8,596 | 0.99× |
| shootout-xblabla20 | 7.7 | 22 | 13.15× | 2,998 | 2,822 | 1.06× |
| hashset | 857 | 904 | 2.64× | 6,580 | 6,604 | 1.18× |
| shootout-xchacha20 | 7.9 | 23 | 12.95× | 8,353 | 8,283 | 0.98× |
| rust-compression | 181 | 365 | 16.61× | 10,158 | 8,206 | 1.59× |
| pulldown-cmark | 59 | 173 | 12.18× | 2,994 | 2,653 | 1.62× |
| gcc-loops | 158 | 608 | 6.95× | 7,877 | 8,175 | 1.33× |
| shootout-fib2 | 6.0 | 19 | 10.88× | 7,965 | 6,579 | 0.90× |
| shootout-sieve | 5.9 | 19 | 12.02× | 8,145 | 8,242 | 0.91× |
| shootout-matrix | 7.7 | 23 | 12.36× | 8,149 | 8,192 | 0.87× |
| shootout-base64 | 7.8 | 25 | 11.60× | 8,049 | 8,028 | 0.88× |
| rust-protobuf | 22 | 121 | 8.50× | 2,767 | 2,635 | 1.18× |
| richards | 1.9 | 7.4 | 10.69× | 913 | 923 | 0.89× |
| shootout-minicsv | 1.1 | 5.8 | 8.73× | 2,007 | 1,761 | 0.84× |
| shootout-ratelimit | 7.5 | 24 | 12.19× | 1,117 | 1,121 | 1.04× |
| rust-json | 41 | 167 | 11.61× | 3,429 | 2,911 | 1.42× |
| rust-html-rewriter | 117 | 382 | 13.36× | 1,376 | 1,589 | 3.76× |
| shootout-ed25519 | 120 | 124 | 19.75× | 8,381 | 7,324 | 1.01× |
| shootout-ctype | 7.6 | 24 | 11.38× | 8,327 | 7,830 | 0.67× |
| shootout-random | 5.8 | 19 | 10.72× | 6,971 | 6,986 | 0.67× |
| blind-sig | 86 | 232 | 14.40× | 10,000 | 7,568 | 0.93× |
| noop | 1.1 | 10 | 5.31× | 1 | 10 | 5.34× |

Aggregates (32 kernels, `noop` excluded): geomean Inst.Lat T2/T1 **3.07×**, Inst.Lat LLVM/T2 **11.9×**, TtV T2/T1 **0.92×**, TtV LLVM/T2 **1.13×**.

## 4. 2dc9ef7b — tier-2 (Walk up + BFS + OSR)

Envs: `TIER2_ENABLE=1`, `TIER2_THRESHOLD=10`, `OSR_THRESHOLD=5000`.
Walk-up + BFS plus OSR back-edge instrumentation. This is the
supported sweep configuration (`notes/design_docs/osr_doc.md` §11).

| Kernel | Tier-1 | Tier-2 | t1/t2 | LLVM/t2 |
|---|---:|---:|---:|---:|
| shootout-ackermann† | 8,421,700 | 4,273,658 | 1.97× | 1.88×† |
| shootout-nestedloop | 5,437,620 | 5,474,120 | 0.99× | 1.62× |
| quicksort | 8,131,002 | 6,659,557 | 1.22× | 1.05× |
| shootout-seqhash | 5,470,318 | 5,504,857 | 0.99× | 1.04× |
| shootout-heapsort | 8,438,098 | 8,065,101 | 1.05× | 1.03× |
| shootout-switch | 8,654,780 | 8,873,744 | 0.98× | 1.02× |
| shootout-matrix | 8,158,768 | 6,760,944 | 1.21× | 1.02× |
| shootout-xblabla20 | 2,877,095 | 2,683,885 | 1.07× | 1.00× |
| shootout-gimli | 8,177,779 | 7,983,140 | 1.02× | 1.00× |
| shootout-xchacha20 | 8,369,227 | 7,730,189 | 1.08× | 1.00× |
| shootout-random | 6,955,889 | 4,400,353 | 1.58× | 1.00× |
| shootout-keccak | 8,229,748 | 6,998,635 | 1.18× | 0.99× |
| blake3-scalar | 5,349,408 | 5,270,062 | 1.02× | 0.99× |
| shootout-base64 | 8,041,983 | 6,831,049 | 1.18× | 0.99× |
| shootout-ratelimit | 1,097,283 | 897,620 | 1.22× | 0.98× |
| bz2 | 7,879,578 | 7,167,222 | 1.10× | 0.98× |
| shootout-memmove | 2,772,908 | 2,732,216 | 1.01× | 0.96× |
| shootout-minicsv | 2,008,480 | 1,478,290 | 1.36× | 0.96× |
| regex | 9,212,065 | 8,964,175 | 1.03× | 0.96× |
| shootout-ctype | 8,279,160 | 5,290,539 | 1.56× | 0.94× |
| hashset | 5,692,391 | 6,113,618 | 0.93× | 0.87× |
| shootout-fib2 | 7,899,479 | 6,563,234 | 1.20× | 0.87× |
| rust-compression | 9,966,981 | 8,376,451 | 1.19× | 0.84× |
| gcc-loops | 7,706,566 | 7,934,002 | 0.97× | 0.83× |
| pulldown-cmark | 2,913,632 | 2,674,922 | 1.09× | 0.82× |
| shootout-sieve | 8,138,509 | 8,806,465 | 0.92× | 0.81× |
| richards | 914,942 | 933,374 | 0.98× | 0.80× |
| rust-protobuf | 2,779,775 | 2,595,864 | 1.07× | 0.79× |
| blind-sig | 9,809,862 | 4,771,271 | 2.06× | 0.76× |
| shootout-ed25519 | 8,286,597 | 6,741,574 | 1.23× | 0.74× |
| rust-json | 3,286,851 | 3,325,374 | 0.99× | 0.65× |
| rust-html-rewriter | 1,269,313 | 1,344,090 | 0.94× | 0.64× |
| noop | 0 | 0 | — | — |

Aggregates (32 kernels, `noop` excluded): geomean t1/t2 **1.144×**, geomean LLVM/t2 **0.942×**.

### Compilation time and time-to-value

Same kernel sort order as the WT table.

| Kernel | Inst.Lat T1 (ms) | Inst.Lat T2 (ms) | LLVM/T2 Inst | TtV T1 (ms) | TtV T2 (ms) | LLVM/T2 TtV |
|---|---:|---:|---:|---:|---:|---:|
| shootout-ackermann | 8.3 | 33 | 9.30× | 6,718 | 5,760 | 0.86× |
| shootout-nestedloop | 6.1 | 23 | 9.30× | 5,460 | 5,470 | 1.66× |
| quicksort | 6.1 | 22 | 9.77× | 8,174 | 6,720 | 1.07× |
| shootout-seqhash | 8.2 | 27 | 11.96× | 5,491 | 5,567 | 1.09× |
| shootout-heapsort | 2.1 | 8.8 | 9.89× | 8,517 | 8,071 | 1.05× |
| shootout-switch | 52 | 57 | 47.98× | 8,802 | 8,904 | 1.32× |
| shootout-matrix | 7.7 | 27 | 10.70× | 8,149 | 6,800 | 1.05× |
| shootout-xblabla20 | 7.7 | 26 | 11.35× | 2,998 | 2,760 | 1.08× |
| shootout-gimli | 0.35 | 4.4 | 3.18× | 8,193 | 7,993 | 1.00× |
| shootout-xchacha20 | 7.9 | 26 | 11.27× | 8,353 | 7,812 | 1.04× |
| shootout-random | 5.8 | 22 | 9.36× | 6,971 | 4,427 | 1.05× |
| shootout-keccak | 2.6 | 6.8 | 54.12× | 8,291 | 6,996 | 1.04× |
| blake3-scalar | 15 | 97 | 7.73× | 5,353 | 5,383 | 1.11× |
| shootout-base64 | 7.8 | 28 | 10.46× | 8,049 | 6,783 | 1.04× |
| shootout-ratelimit | 7.5 | 28 | 10.39× | 1,117 | 940 | 1.24× |
| bz2 | 36 | 83 | 13.93× | 7,964 | 7,289 | 1.11× |
| shootout-memmove | 7.6 | 26 | 11.31× | 2,742 | 2,701 | 1.10× |
| shootout-minicsv | 1.1 | 6.2 | 8.23× | 2,007 | 1,494 | 0.99× |
| regex | 125 | 409 | 13.22× | 9,460 | 9,451 | 1.49× |
| shootout-ctype | 7.6 | 27 | 10.11× | 8,327 | 5,273 | 1.00× |
| hashset | 857 | 414 | 5.77× | 6,580 | 6,538 | 1.19× |
| shootout-fib2 | 6.0 | 22 | 9.59× | 7,965 | 6,613 | 0.89× |
| rust-compression | 181 | 420 | 14.44× | 10,158 | 8,872 | 1.47× |
| gcc-loops | 158 | 675 | 6.26× | 7,877 | 8,561 | 1.27× |
| pulldown-cmark | 59 | 191 | 11.04× | 2,994 | 2,880 | 1.49× |
| shootout-sieve | 5.9 | 22 | 10.41× | 8,145 | 8,807 | 0.86× |
| richards | 1.9 | 8.6 | 9.19× | 913 | 949 | 0.87× |
| rust-protobuf | 22 | 130 | 7.92× | 2,767 | 2,781 | 1.11× |
| blind-sig | 86 | 262 | 12.77× | 10,000 | 4,894 | 1.44× |
| shootout-ed25519 | 120 | 129 | 19.02× | 8,381 | 6,896 | 1.08× |
| rust-json | 41 | 178 | 10.92× | 3,429 | 3,485 | 1.19× |
| rust-html-rewriter | 117 | 425 | 12.01× | 1,376 | 1,806 | 3.31× |
| noop | 1.1 | 11 | 5.00× | 1 | 11 | 5.09× |

Aggregates (32 kernels, `noop` excluded): geomean Inst.Lat T2/T1 **3.33×**, Inst.Lat LLVM/T2 **11.0×**, TtV T2/T1 **0.90×**, TtV LLVM/T2 **1.16×**.

---

**§3 vs §4 — what OSR adds at the current head.** §3 is regular
tier-2 with `OSR_THRESHOLD=0`; §4 adds `OSR_THRESHOLD=5000`. The
OSR back-edge instrumentation is paid in tier-1 (every outermost
loop in every defined function gets the sentinel diamond plus
locals snapshot), so §4's Inst.Lat T2 grows over §3 — geomean
Inst.Lat T2/T1 in §3 is **3.07×** vs
**3.33×** in §4. In return, OSR rescues
long-running loops that started under tier-1: blind-sig (§3 t2 =
7.3 M µs → §4 t2 = 4.8 M µs) and shootout-ctype (§3 t2 = 7.8 M →
§4 t2 = 5.3 M) are the largest swings. End-to-end TtV is also
better with OSR: geomean TtV LLVM/T2 **1.13×**
in §3 vs **1.16×** in §4 — OSR
converts the longer tier-1 warm-up tail into earlier tier-2 entry.

†`shootout-ackermann` is bimodal on this hardware; the LLVM/t2
ratio reflects a clean-mode median rather than a structural
improvement. The marker is applied wherever an ackermann ratio
appears.
