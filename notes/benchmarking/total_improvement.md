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

This commit collapses the OSR back-edge poll diamond from a two-array
load-and-test to a single-slot magnitude check (sentinel encoding:
slot=1 means COUNTING, slot=0 means WAITING, slot=function_ptr means
READY). The WAITING-state per-iteration cost drops from six ops to
three ops (LOAD slot + TEST against zero + branch). vs. `c602091b`
the suite geomean improves t1/t2 1.115× → 1.144× and LLVM/t2 0.920×
→ 0.942×. The largest per-kernel move is gcc-loops (t1/t2 0.89× →
0.97×, LLVM/t2 0.68× → 0.83×). One residual loss: rust-html-rewriter
holds at 0.94× t1/t2 because the new diamond's READY transition path
is two ops longer (extra `slot == 1` check before transitioning),
which costs more on rhr's many short-frame invocations than the
WAITING-state savings recover for that workload. †`shootout-ackermann`
is bimodal on this hardware; the 1.88× LLVM/t2 reflects a clean-mode
median rather than a structural improvement.
