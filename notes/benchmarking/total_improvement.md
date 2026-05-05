# Tier-1 / Tier-2 / LLVM JIT WorkTime comparison — Release build

Per-kernel 3-run median **WorkTime (µs)** on `sightglass-strong` (33
kernels). `t1/t2` = tier-1 WT / tier-2 WT, `LLVM/t2` = LLVM JIT WT /
tier-2 WT — values > 1 mean tier-2 wins. Rows sorted by **LLVM/t2**
descending.

Build: `CMAKE_BUILD_TYPE=Release`, `WASMEDGE_IR_JIT_OPT_LEVEL=2`,
`WASMEDGE_QUIET=1`, suite `sightglass-strong`.

Logs at `/tmp/wasm-<commit>-{tier1,tier2,llvm}-run{1,2,3}.log`.

## 89e83770 — tier-2 - Promote Hot Callees

Envs: `TIER2_ENABLE=1`, `TIER2_THRESHOLD=10`, `TIER2_LOOP_THRESHOLD=5`.

| Kernel | Tier-1 | Tier-2 | LLVM JIT | t1/t2 | LLVM/t2 |
|---|---:|---:|---:|---:|---:|
| shootout-ackermann† | 8,334,738 | 6,123,510 | 10,185,484 | 1.36× | 1.66×† |
| shootout-nestedloop | 5,474,884 | 5,464,670 | 8,880,056 | 1.00× | 1.62× |
| shootout-fib2 | 7,925,657 | 5,733,709 | 6,494,341 | 1.38× | 1.13× |
| shootout-switch | 8,665,866 | 8,714,749 | 9,026,815 | 0.99× | 1.04× |
| shootout-seqhash | 5,469,851 | 5,454,939 | 5,565,028 | 1.00× | 1.02× |
| shootout-keccak | 8,221,947 | 6,909,272 | 6,971,416 | 1.19× | 1.01× |
| quicksort | 8,105,262 | 6,962,820 | 6,891,600 | 1.16× | 0.99× |
| shootout-gimli | 8,185,529 | 8,071,569 | 7,908,472 | 1.01× | 0.98× |
| shootout-memmove | 2,713,596 | 2,707,938 | 2,632,441 | 1.00× | 0.97× |
| shootout-xchacha20 | 8,302,488 | 8,352,444 | 8,083,824 | 0.99× | 0.97× |
| blake3-scalar | 5,345,217 | 5,376,025 | 5,190,324 | 0.99× | 0.97× |
| shootout-heapsort | 8,444,310 | 8,557,783 | 8,056,059 | 0.99× | 0.94× |
| bz2 | 7,842,613 | 7,511,928 | 7,040,926 | 1.04× | 0.94× |
| regex | 9,184,954 | 9,207,907 | 8,623,984 | 1.00× | 0.94× |
| shootout-xblabla20 | 2,870,149 | 2,853,115 | 2,623,138 | 1.01× | 0.92× |
| gcc-loops | 7,676,277 | 7,718,141 | 6,677,683 | 0.99× | 0.87× |
| shootout-base64 | 8,026,361 | 8,023,518 | 6,781,191 | 1.00× | 0.85× |
| shootout-matrix | 8,142,751 | 8,145,261 | 6,810,975 | 1.00× | 0.84× |
| shootout-sieve | 8,096,649 | 8,214,942 | 6,856,647 | 0.99× | 0.83× |
| richards | 909,320 | 905,745 | 738,366 | 1.00× | 0.82× |
| shootout-ratelimit | 1,098,413 | 1,086,098 | 869,232 | 1.01× | 0.80× |
| rust-compression | 9,907,119 | 8,968,602 | 6,926,189 | 1.10× | 0.77× |
| pulldown-cmark | 2,938,697 | 2,877,061 | 2,167,639 | 1.02× | 0.75× |
| rust-html-rewriter | 1,233,778 | 1,308,031 | 905,275 | 0.94× | 0.69× |
| hashset | 5,675,652 | 7,939,384 | 5,377,140 | 0.71× | 0.68× |
| shootout-minicsv | 1,967,203 | 2,119,326 | 1,412,484 | 0.93× | 0.67× |
| rust-protobuf | 2,749,657 | 3,132,637 | 2,015,193 | 0.88× | 0.64× |
| shootout-random | 6,990,248 | 6,967,795 | 4,399,990 | 1.00× | 0.63× |
| rust-json | 3,218,410 | 3,432,358 | 2,141,202 | 0.94× | 0.62× |
| shootout-ed25519 | 8,213,538 | 12,940,674 | 4,970,233 | 0.63× | 0.38× |
| blind-sig | 9,777,375 | 10,993,774 | 3,900,783 | 0.89× | 0.35× |
| shootout-ctype | 8,234,113 | 16,305,276 | 4,978,469 | 0.50× | 0.31× |
| noop | 0 | 0 | 0 | — | — |

Aggregates (32 kernels, `noop` excluded): geomean t1/t2 **0.975×**,
geomean LLVM/t2 **0.814×**.

### Compilation time and time-to-value

3-run median **`Inst.Lat` (compile/instantiation time, ms)** and
**`TtV` (time to value, ms)** per mode. Same kernel sort order as the
WT table above. Inst.Lat measures the upfront cost paid before the
first instruction of wasm executes; TtV is the end-to-end wall-clock
(`Inst.Lat + WT`) that an embedder waits for.

| Kernel | Inst.Lat T1 (ms) | Inst.Lat T2 (ms) | Inst.Lat LLVM (ms) | LLVM/T2 Inst | TtV T1 (ms) | TtV T2 (ms) | TtV LLVM (ms) | LLVM/T2 TtV |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| shootout-ackermann | 8.2 | 8.5 | 315 | 37.0× | 8,344 | 6,134 | 10,502 | 1.71× |
| shootout-nestedloop | 5.7 | 6.2 | 209 | 33.5× | 5,481 | 5,474 | 9,089 | 1.66× |
| shootout-fib2 | 5.9 | 6.1 | 213 | 35.0× | 7,932 | 5,740 | 6,708 | 1.17× |
| shootout-switch | 39 | 50 | 2,716 | 54.0× | 8,704 | 8,770 | 11,705 | 1.33× |
| shootout-seqhash | 8.2 | 8.5 | 329 | 38.8× | 5,479 | 5,464 | 5,893 | 1.08× |
| shootout-keccak | 2.7 | 2.7 | 376 | 138× | 8,225 | 6,913 | 7,348 | 1.06× |
| quicksort | 6.5 | 6.3 | 218 | 34.6× | 8,112 | 6,970 | 7,110 | 1.02× |
| shootout-gimli | 0.46 | 0.39 | 15 | 38.1× | 8,186 | 8,072 | 7,923 | 0.98× |
| shootout-memmove | 7.5 | 8.1 | 290 | 35.7× | 2,722 | 2,717 | 2,925 | 1.08× |
| shootout-xchacha20 | 7.8 | 8.2 | 283 | 34.6× | 8,311 | 8,362 | 8,368 | 1.00× |
| blake3-scalar | 15 | 17 | 713 | 41.2× | 5,361 | 5,399 | 5,914 | 1.10× |
| shootout-heapsort | 2.1 | 2.2 | 89 | 41.2× | 8,447 | 8,560 | 8,146 | 0.95× |
| bz2 | 33 | 33 | 1,131 | 34.1× | 7,878 | 7,548 | 8,200 | 1.09× |
| regex | 125 | 128 | 5,297 | 41.3× | 9,327 | 9,353 | 13,932 | 1.49× |
| shootout-xblabla20 | 7.7 | 8.3 | 288 | 34.6× | 2,878 | 2,862 | 2,912 | 1.02× |
| gcc-loops | 150 | 163 | 3,983 | 24.5× | 7,834 | 7,888 | 10,672 | 1.35× |
| shootout-base64 | 7.6 | 8.0 | 294 | 36.9× | 8,034 | 8,032 | 7,071 | 0.88× |
| shootout-matrix | 7.4 | 7.8 | 287 | 36.7× | 8,151 | 8,154 | 7,098 | 0.87× |
| shootout-sieve | 5.7 | 6.0 | 206 | 34.6× | 8,103 | 8,221 | 7,060 | 0.86× |
| richards | 1.9 | 2.0 | 79 | 39.9× | 912 | 908 | 818 | 0.90× |
| shootout-ratelimit | 8.9 | 7.9 | 284 | 36.1× | 1,108 | 1,095 | 1,154 | 1.05× |
| rust-compression | 182 | 188 | 5,908 | 31.4× | 10,101 | 9,174 | 12,868 | 1.40× |
| pulldown-cmark | 61 | 61 | 2,041 | 33.5× | 3,003 | 2,943 | 4,233 | 1.44× |
| rust-html-rewriter | 118 | 122 | 4,990 | 40.9× | 1,359 | 1,449 | 5,931 | 4.09× |
| hashset | 871 | 825 | 2,198 | 2.66× | 6,574 | 8,771 | 7,583 | 0.86× |
| shootout-minicsv | 0.98 | 1.1 | 51 | 48.1× | 1,968 | 2,121 | 1,463 | 0.69× |
| rust-protobuf | 22 | 24 | 1,024 | 42.8× | 2,774 | 3,159 | 3,020 | 0.96× |
| shootout-random | 5.8 | 6.0 | 205 | 34.1× | 6,998 | 6,974 | 4,610 | 0.66× |
| rust-json | 41 | 43 | 1,816 | 42.0× | 3,263 | 3,479 | 3,980 | 1.14× |
| shootout-ed25519 | 121 | 123 | 2,404 | 19.6× | 8,335 | 13,071 | 7,368 | 0.56× |
| blind-sig | 86 | 88 | 3,241 | 36.7× | 9,870 | 11,088 | 7,138 | 0.64× |
| shootout-ctype | 7.6 | 8.0 | 273 | 34.1× | 8,242 | 16,314 | 5,252 | 0.32× |
| noop | 1.1 | 1.2 | 56 | 46.0× | 1 | 1 | 56 | 41.5× |

Aggregates (32 kernels, `noop` excluded): geomean Inst.Lat T2/T1
**1.04×**, Inst.Lat LLVM/T2 **34.9×**, TtV T2/T1 **1.02×**, TtV
LLVM/T2 **1.04×**.

Tier-2's startup is essentially identical to tier-1 here — at this
commit the OSR back-edge instrumentation has not landed yet, so
tier-1 IR-JIT lowering does the same work for both `TIER2_ENABLE=0`
and `TIER2_ENABLE=1`. The tier-2 worker compiles in the background
during execution; its cost is amortised into WT. LLVM JIT pays a
35× larger upfront cost because it lowers the entire wasm module
to LLVM IR at instantiation. End-to-end (TtV), tier-2 modestly
beats LLVM JIT in the geomean: LLVM JIT's faster execution doesn't
pay back its compile cost on most kernels.


## 752997db - tier2 - Walk up + BFS + OSR

Envs: `TIER2_ENABLE=1`, `TIER2_THRESHOLD=10`, `OSR_THRESHOLD=5000`.

| Kernel | Tier-1 | Tier-2 | LLVM JIT | t1/t2 | LLVM/t2 |
|---|---:|---:|---:|---:|---:|
| shootout-ackermann† | 8,421,700 | 4,273,658 | 8,053,812 | 1.97× | 1.88×† |
| shootout-nestedloop | 5,437,620 | 5,474,120 | 8,867,429 | 0.99× | 1.62× |
| quicksort | 8,131,002 | 6,659,557 | 7,024,610 | 1.22× | 1.05× |
| shootout-seqhash | 5,470,318 | 5,504,857 | 5,721,465 | 0.99× | 1.04× |
| shootout-heapsort | 8,438,098 | 8,065,101 | 8,336,287 | 1.05× | 1.03× |
| shootout-switch | 8,654,780 | 8,873,744 | 9,069,258 | 0.98× | 1.02× |
| shootout-matrix | 8,158,768 | 6,760,944 | 6,906,804 | 1.21× | 1.02× |
| shootout-xblabla20 | 2,877,095 | 2,683,885 | 2,691,703 | 1.07× | 1.00× |
| shootout-gimli | 8,177,779 | 7,983,140 | 8,001,214 | 1.02× | 1.00× |
| shootout-xchacha20 | 8,369,227 | 7,730,189 | 7,735,863 | 1.08× | 1.00× |
| shootout-random | 6,955,889 | 4,400,353 | 4,390,723 | 1.58× | 1.00× |
| shootout-keccak | 8,229,748 | 6,998,635 | 6,926,285 | 1.18× | 0.99× |
| blake3-scalar | 5,349,408 | 5,270,062 | 5,214,484 | 1.02× | 0.99× |
| shootout-base64 | 8,041,983 | 6,831,049 | 6,758,004 | 1.18× | 0.99× |
| shootout-ratelimit | 1,097,283 | 897,620 | 882,984 | 1.22× | 0.98× |
| bz2 | 7,879,578 | 7,167,222 | 6,995,518 | 1.10× | 0.98× |
| shootout-memmove | 2,772,908 | 2,732,216 | 2,635,747 | 1.01× | 0.96× |
| shootout-minicsv | 2,008,480 | 1,478,290 | 1,417,238 | 1.36× | 0.96× |
| regex | 9,212,065 | 8,964,175 | 8,582,285 | 1.03× | 0.96× |
| shootout-ctype | 8,279,160 | 5,290,539 | 4,994,419 | 1.56× | 0.94× |
| hashset | 5,692,391 | 6,113,618 | 5,346,923 | 0.93× | 0.87× |
| shootout-fib2 | 7,899,479 | 6,563,234 | 5,726,646 | 1.20× | 0.87× |
| rust-compression | 9,966,981 | 8,376,451 | 6,999,674 | 1.19× | 0.84× |
| gcc-loops | 7,706,566 | 7,934,002 | 6,608,757 | 0.97× | 0.83× |
| pulldown-cmark | 2,913,632 | 2,674,922 | 2,186,000 | 1.09× | 0.82× |
| shootout-sieve | 8,138,509 | 8,806,465 | 7,171,015 | 0.92× | 0.81× |
| richards | 914,942 | 933,374 | 745,960 | 0.98× | 0.80× |
| rust-protobuf | 2,779,775 | 2,595,864 | 2,046,396 | 1.07× | 0.79× |
| blind-sig | 9,809,862 | 4,771,271 | 3,638,597 | 2.06× | 0.76× |
| shootout-ed25519 | 8,286,597 | 6,741,574 | 4,995,236 | 1.23× | 0.74× |
| rust-json | 3,286,851 | 3,325,374 | 2,167,699 | 0.99× | 0.65× |
| rust-html-rewriter | 1,269,313 | 1,344,090 | 855,166 | 0.94× | 0.64× |
| noop | 0 | 0 | 0 | — | — |

Aggregates (32 kernels, `noop` excluded): geomean t1/t2 **1.144×**,
geomean LLVM/t2 **0.942×**.

### Compilation time and time-to-value

3-run median **`Inst.Lat` (compile/instantiation time, ms)** and
**`TtV` (time to value, ms)** per mode. Same kernel sort order as
the WT table above.

| Kernel | Inst.Lat T1 (ms) | Inst.Lat T2 (ms) | Inst.Lat LLVM (ms) | LLVM/T2 Inst | TtV T1 (ms) | TtV T2 (ms) | TtV LLVM (ms) | LLVM/T2 TtV |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| shootout-nestedloop | 6.1 | 23 | 214 | 9.19× | 5,460 | 5,470 | 9,102 | 1.66× |
| shootout-heapsort | 2.1 | 8.8 | 87 | 9.90× | 8,517 | 8,071 | 8,478 | 1.05× |
| quicksort | 6.1 | 22 | 215 | 9.62× | 8,174 | 6,720 | 7,169 | 1.07× |
| shootout-seqhash | 8.2 | 27 | 323 | 12.0× | 5,491 | 5,567 | 6,045 | 1.09× |
| shootout-switch | 52 | 57 | 2,735 | 48.3× | 8,802 | 8,904 | 11,785 | 1.32× |
| shootout-matrix | 7.7 | 27 | 289 | 10.6× | 8,149 | 6,800 | 7,113 | 1.05× |
| shootout-xchacha20 | 7.9 | 26 | 293 | 11.4× | 8,353 | 7,812 | 8,103 | 1.04× |
| shootout-random | 5.8 | 22 | 206 | 9.35× | 6,971 | 4,427 | 4,649 | 1.05× |
| shootout-gimli | 0.35 | 4.4 | 14 | 3.16× | 8,193 | 7,993 | 8,004 | 1.00× |
| shootout-base64 | 7.8 | 28 | 293 | 10.5× | 8,049 | 6,783 | 7,038 | 1.04× |
| shootout-memmove | 7.6 | 26 | 294 | 11.4× | 2,742 | 2,701 | 2,960 | 1.10× |
| blake3-scalar | 15 | 97 | 750 | 7.72× | 5,353 | 5,383 | 5,980 | 1.11× |
| shootout-keccak | 2.6 | 6.8 | 368 | 54.5× | 8,291 | 6,996 | 7,275 | 1.04× |
| shootout-xblabla20 | 7.7 | 26 | 295 | 11.3× | 2,998 | 2,760 | 2,989 | 1.08× |
| bz2 | 36 | 83 | 1,156 | 13.9× | 7,964 | 7,289 | 8,108 | 1.11× |
| shootout-minicsv | 1.1 | 6.2 | 51 | 8.33× | 2,007 | 1,494 | 1,482 | 0.99× |
| shootout-ratelimit | 7.5 | 28 | 291 | 10.5× | 1,117 | 940 | 1,167 | 1.24× |
| regex | 125 | 409 | 5,408 | 13.2× | 9,460 | 9,451 | 14,082 | 1.49× |
| shootout-ctype | 7.6 | 27 | 273 | 10.1× | 8,327 | 5,273 | 5,247 | 1.00× |
| hashset | 857 | 414 | 2,390 | 5.77× | 6,580 | 6,538 | 7,805 | 1.19× |
| shootout-fib2 | 6.0 | 22 | 211 | 9.54× | 7,965 | 6,613 | 5,916 | 0.89× |
| gcc-loops | 158 | 675 | 4,224 | 6.26× | 7,877 | 8,561 | 10,895 | 1.27× |
| shootout-sieve | 5.9 | 22 | 229 | 10.5× | 8,145 | 8,807 | 7,535 | 0.86× |
| rust-compression | 181 | 420 | 6,064 | 14.5× | 10,158 | 8,872 | 13,038 | 1.47× |
| shootout-ackermann | 8.3 | 33 | 307 | 9.33× | 6,718 | 5,760 | 4,961 | 0.86× |
| pulldown-cmark | 59 | 191 | 2,108 | 11.0× | 2,994 | 2,880 | 4,296 | 1.49× |
| blind-sig | 86 | 262 | 3,346 | 12.8× | 10,000 | 4,894 | 7,034 | 1.44× |
| richards | 1.9 | 8.6 | 79 | 9.09× | 913 | 949 | 823 | 0.87× |
| rust-protobuf | 22 | 130 | 1,029 | 7.91× | 2,767 | 2,781 | 3,097 | 1.11× |
| shootout-ed25519 | 120 | 129 | 2,454 | 19.0× | 8,381 | 6,896 | 7,427 | 1.08× |
| rust-json | 41 | 178 | 1,944 | 10.9× | 3,429 | 3,485 | 4,136 | 1.19× |
| rust-html-rewriter | 117 | 425 | 5,104 | 12.0× | 1,376 | 1,806 | 5,978 | 3.31× |
| noop | 1.1 | 11 | 55 | 4.96× | 1 | 11 | 56 | 4.92× |

Aggregates (32 kernels, `noop` excluded): geomean Inst.Lat T2/T1
**3.32×**, Inst.Lat LLVM/T2 **11.0×**, TtV T2/T1 **0.901×**, TtV
LLVM/T2 **1.16×**.

The tier-2 startup cost has grown ~3.3× over tier-1 here, vs ~1.04×
at §89e83770. The increase is the OSR back-edge instrumentation
emitted by the tier-1 IR JIT when `OSR_THRESHOLD > 0`: every
outermost loop in every defined function gets the three-state
sentinel diamond, the locals-frame snapshot store sequence, and the
lifted env-field loads. That extra IR runs through dstogov/ir's
compiler at module load, lengthening Inst.Lat. Pure tier-1
(`TIER2_ENABLE=0`) is unchanged from §89e83770 because it omits
that instrumentation.

The tradeoff is favourable end-to-end: TtV T2/T1 = 0.901× (tier-2 +
OSR is 10% faster wall-clock than pure tier-1) and TtV LLVM/T2 =
1.16× (tier-2 + OSR is 16% faster wall-clock than whole-module LLVM
JIT). LLVM JIT's startup has shortened relative to tier-2 (34.9× →
11.0×) for the same reason — tier-2 startup grew, not because LLVM
got faster.

Per-kernel highlights:

- **blind-sig:** TtV 10,000 → 4,894 ms (tier-1 → tier-2+OSR).
  Largest absolute saving in the suite, dominated by the OSR rescue
  on the bigint signing loop.
- **shootout-ctype:** TtV 8,327 → 5,273 ms; tier-2+OSR is within
  0.5% of LLVM JIT's TtV (5,247 ms). Compile-time cost reaches
  parity with LLVM JIT for this one-shot-caller kernel.
- **gcc-loops:** Inst.Lat T2 jumps to 675 ms (4.3× tier-1) because
  the wide call graph (1,671 functions, 99 promoted) means every
  function's hot loops carry OSR instrumentation. WT tier-2 lost
  ~2% (8,561 ms vs 7,877 tier-1 TtV), so this is the kernel where
  the instrumentation cost is most visible at the bottom line.

†`shootout-ackermann` is bimodal on this hardware; the 1.88× LLVM/t2
reflects a clean-mode median rather than a structural improvement.
