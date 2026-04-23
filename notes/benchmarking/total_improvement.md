# Tier-1 / Tier-2 / LLVM JIT WorkTime comparison — Release build

Per-kernel 3-run median **WorkTime (µs)** on `sightglass-strong` (33
kernels). `t1/t2` = tier-1 WT / tier-2 WT, `LLVM/t2` = LLVM JIT WT /
tier-2 WT — values > 1 mean tier-2 wins. Rows sorted by **LLVM/t2**
descending.

Build: `CMAKE_BUILD_TYPE=Release`, `WASMEDGE_IR_JIT_OPT_LEVEL=2`,
`WASMEDGE_QUIET=1`, suite `sightglass-strong`.

Logs at `/tmp/wasm-<commit>-{tier1,tier2,llvm}-run{1,2,3}.log`.

## 89e83770 — tier-2 V2, pre-OSR

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
| noop | 0 | 0 | 0 | — | — |
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

Aggregates (32 kernels, `noop` excluded): geomean t1/t2 **0.975×**,
geomean LLVM/t2 **0.814×**.


## c602091b - tier2 + OSR

Envs: `TIER2_ENABLE=1`, `TIER2_THRESHOLD=10`, `OSR_THRESHOLD=5000`.

| Kernel | Tier-1 | Tier-2 | LLVM JIT | t1/t2 | LLVM/t2 |
|---|---:|---:|---:|---:|---:|
| shootout-nestedloop | 5,464,326 | 5,469,977 | 8,863,836 | 1.00× | 1.62× |
| shootout-seqhash | 5,483,990 | 5,432,083 | 5,703,767 | 1.01× | 1.05× |
| quicksort | 8,122,251 | 6,592,672 | 6,893,647 | 1.23× | 1.05× |
| shootout-heapsort | 8,499,003 | 8,028,295 | 8,355,503 | 1.06× | 1.04× |
| shootout-matrix | 8,132,709 | 6,743,591 | 6,812,769 | 1.21× | 1.01× |
| shootout-switch | 8,648,764 | 8,912,738 | 8,974,158 | 0.97× | 1.01× |
| shootout-xchacha20 | 8,286,913 | 7,743,770 | 7,766,690 | 1.07× | 1.00× |
| shootout-gimli | 8,143,864 | 7,952,730 | 7,974,968 | 1.02× | 1.00× |
| shootout-keccak | 8,208,370 | 6,925,288 | 6,934,385 | 1.19× | 1.00× |
| shootout-random | 6,959,567 | 4,401,707 | 4,391,915 | 1.58× | 1.00× |
| shootout-base64 | 8,024,013 | 6,760,734 | 6,735,372 | 1.19× | 1.00× |
| shootout-xblabla20 | 2,881,032 | 2,676,474 | 2,659,948 | 1.08× | 0.99× |
| blake3-scalar | 5,348,861 | 5,245,053 | 5,195,974 | 1.02× | 0.99× |
| shootout-memmove | 2,731,678 | 2,699,363 | 2,632,500 | 1.01× | 0.98× |
| shootout-ackermann† | 5,991,837 | 6,017,530 | 5,826,280 | 1.00× | 0.97×† |
| shootout-ratelimit | 1,088,355 | 899,550 | 866,274 | 1.21× | 0.96× |
| regex | 9,221,433 | 8,989,311 | 8,584,876 | 1.03× | 0.96× |
| shootout-minicsv | 1,988,147 | 1,484,873 | 1,414,358 | 1.34× | 0.95× |
| bz2 | 7,801,915 | 7,280,345 | 6,933,445 | 1.07× | 0.95× |
| shootout-ctype | 8,224,391 | 5,254,913 | 4,993,191 | 1.57× | 0.95× |
| hashset | 5,634,203 | 6,078,929 | 5,351,455 | 0.93× | 0.88× |
| shootout-fib2 | 7,901,672 | 6,533,942 | 5,729,634 | 1.21× | 0.88× |
| rust-compression | 9,855,091 | 8,418,836 | 6,997,809 | 1.17× | 0.83× |
| shootout-sieve | 8,097,934 | 8,751,565 | 7,187,861 | 0.93× | 0.82× |
| pulldown-cmark | 2,930,983 | 2,680,909 | 2,175,856 | 1.09× | 0.81× |
| richards | 892,206 | 916,979 | 729,915 | 0.97× | 0.80× |
| gcc-loops | 7,661,647 | 8,569,409 | 6,719,074 | 0.89× | 0.78× |
| rust-protobuf | 2,722,612 | 2,621,429 | 2,043,853 | 1.04× | 0.78× |
| blind-sig | 9,734,972 | 4,716,495 | 3,632,225 | 2.06× | 0.77× |
| shootout-ed25519 | 8,206,512 | 6,695,313 | 4,931,879 | 1.23× | 0.74× |
| rust-json | 3,255,009 | 3,339,177 | 2,200,312 | 0.97× | 0.66× |
| rust-html-rewriter | 1,244,195 | 1,315,183 | 854,799 | 0.95× | 0.65× |
| noop | 0 | 0 | 0 | — | — |

Aggregates (32 kernels, `noop` excluded): geomean t1/t2 **1.115×**,
geomean LLVM/t2 **0.920×**.

†`shootout-ackermann` is bimodal on this hardware (deep-recursion
branch-predictor pathology). Per-run WT:
tier-1 5,303k / 5,992k / 6,964k; tier-2 5,289k / 6,705k / 6,018k;
LLVM 5,826k / 5,435k / 6,725k. All three columns show ~25%
run-to-run swing; this run of the sweep happened to land the medians
close together. The 1.00× t1/t2 and 0.97× LLVM/t2 entries are within
ackermann's natural noise, not a structural signal.
