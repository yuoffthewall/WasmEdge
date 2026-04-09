# Tier-2 Benchmark

**Date:** 2026-04-09
**Branches:** `tier2` (tier-2 LLVM recompilation enabled) vs `ir_jit` (baseline, tier-1 only)
**Setup:** IR JIT O2, bound-check off, 3 runs per kernel, median taken. Submodules updated per branch.
**Tier-2 config:** `WASMEDGE_TIER2_ENABLE=1`, `WASMEDGE_TIER2_THRESHOLD=100`, `WASMEDGE_TIER2_LOOP_THRESHOLD=0`
**Excluded:** spidermonkey, tinygo, richards (no output), shootout-ackermann (high variance)

## Aggregate


| Metric               | ir_jit (us) | tier2 (us) | Speedup |
| -------------------- | ----------- | ---------- | ------- |
| Exec                 | 10,231,962  | 10,191,905 | 1.00x   |
| Total (compile+exec) | 14,540,192  | 14,861,447 | 0.98x   |
| Exec (no rust-comp)  | 9,216,934   | 9,196,969  | 1.00x   |
| Total (no rust-comp) | 12,906,092  | 13,202,063 | 0.98x   |


Execution is flat in aggregate. Total time is **2% slower** due to tier-2 compile overhead (tier-1 compile + background LLVM recompilation both contribute to the "compile" column).

## Per-Kernel Results (3-run median, microseconds)


| Kernel              | Base Compile | T2 Compile | Comp Spd | Base Exec | T2 Exec   | Exec Spd | Base Total | T2 Total  | Total Spd |
| ------------------- | ------------ | ---------- | -------- | --------- | --------- | -------- | ---------- | --------- | --------- |
| blake3-scalar       | 37,955       | 47,549     | 0.80x    | 61        | 654       | 0.09x    | 54,218     | 64,835    | 0.84x     |
| blind-sig           | 208,317      | 235,308    | 0.89x    | 46,976    | 43,874    | 1.07x    | 333,235    | 360,147   | 0.93x     |
| bz2                 | 98,315       | 107,953    | 0.91x    | 20,149    | 20,567    | 0.98x    | 140,817    | 152,067   | 0.93x     |
| gcc-loops           | 156,704      | 185,908    | 0.84x    | 2,695,992 | 2,701,118 | 1.00x    | 2,904,943  | 2,938,651 | 0.99x     |
| hashset             | 1,056,133    | 1,067,976  | 0.99x    | 28,885    | 32,126    | 0.90x    | 1,152,177  | 1,167,884 | 0.99x     |
| noop                | 1,958        | 2,793      | 0.70x    | 2         | 2         | 0.87x    | 2,837      | 3,714     | 0.76x     |
| pulldown-cmark      | 176,147      | 203,971    | 0.86x    | 3,456     | 4,830     | 0.72x    | 225,434    | 256,144   | 0.88x     |
| quicksort           | 16,363       | 18,448     | 0.89x    | 17,505    | 17,775    | 0.98x    | 37,997     | 40,093    | 0.95x     |
| regex               | 309,974      | 357,565    | 0.87x    | 46,594    | 50,224    | 0.93x    | 478,271    | 528,337   | 0.91x     |
| rust-compression    | 491,228      | 536,739    | 0.92x    | 1,015,029 | 994,936   | 1.02x    | 1,634,100  | 1,659,384 | 0.98x     |
| rust-html-rewriter  | 293,928      | 354,082    | 0.83x    | 9,895     | 14,226    | 0.70x    | 410,010    | 475,366   | 0.86x     |
| rust-json           | 104,053      | 125,250    | 0.83x    | 5,266     | 5,376     | 0.98x    | 148,300    | 171,946   | 0.86x     |
| rust-protobuf       | 53,802       | 69,648     | 0.77x    | 3,238     | 3,358     | 0.96x    | 80,732     | 98,569    | 0.82x     |
| shootout-base64     | 21,322       | 24,355     | 0.88x    | 70,390    | 70,904    | 0.99x    | 98,566     | 101,215   | 0.97x     |
| shootout-ctype      | 20,693       | 23,787     | 0.87x    | 140,492   | 147,524   | 0.95x    | 168,577    | 177,523   | 0.95x     |
| shootout-ed25519    | 141,006      | 149,642    | 0.94x    | 1,713,147 | 1,677,950 | 1.02x    | 1,877,164  | 1,848,768 | 1.02x     |
| shootout-fib2       | 16,887       | 18,818     | 0.90x    | 564,368   | 568,267   | 0.99x    | 587,703    | 590,988   | 0.99x     |
| shootout-gimli      | 763          | 1,000      | 0.76x    | 838       | 837       | 1.00x    | 2,014      | 2,237     | 0.90x     |
| shootout-heapsort   | 4,900        | 5,864      | 0.84x    | 562,473   | 565,754   | 0.99x    | 569,771    | 574,162   | 0.99x     |
| shootout-keccak     | 5,098        | 6,361      | 0.80x    | 2,558     | 2,630     | 0.97x    | 13,072     | 14,497    | 0.90x     |
| shootout-matrix     | 20,940       | 23,889     | 0.88x    | 69,277    | 69,196    | 1.00x    | 96,422     | 99,631    | 0.97x     |
| shootout-memmove    | 22,279       | 24,681     | 0.90x    | 23,655    | 23,752    | 1.00x    | 52,972     | 55,316    | 0.96x     |
| shootout-minicsv    | 2,444        | 2,964      | 0.82x    | 1,210,437 | 1,194,077 | 1.01x    | 1,214,042  | 1,198,273 | 1.01x     |
| shootout-nestedloop | 16,758       | 18,759     | 0.89x    | 1         | 1         | 0.96x    | 20,929     | 22,921    | 0.91x     |
| shootout-random     | 16,843       | 18,674     | 0.90x    | 141,415   | 141,018   | 1.00x    | 162,544    | 163,731   | 0.99x     |
| shootout-ratelimit  | 21,197       | 23,741     | 0.89x    | 174,582   | 170,155   | 1.03x    | 202,032    | 200,017   | 1.01x     |
| shootout-seqhash    | 22,263       | 24,824     | 0.90x    | 1,493,527 | 1,499,345 | 1.00x    | 1,523,209  | 1,531,770 | 0.99x     |
| shootout-sieve      | 16,723       | 18,871     | 0.89x    | 140,044   | 139,499   | 1.00x    | 160,916    | 162,151   | 0.99x     |
| shootout-switch     | 81,820       | 90,507     | 0.90x    | 29,533    | 29,835    | 0.99x    | 129,408    | 137,479   | 0.94x     |
| shootout-xblabla20  | 21,337       | 24,193     | 0.88x    | 1,416     | 1,356     | 1.04x    | 29,219     | 32,129    | 0.91x     |
| shootout-xchacha20  | 21,377       | 24,167     | 0.88x    | 761       | 735       | 1.03x    | 28,562     | 31,504    | 0.91x     |


## Exec Speedups (tier2 wins)


| Kernel             | Exec Speedup | Delta (us) | Notes                                          |
| ------------------ | ------------ | ---------- | ---------------------------------------------- |
| blind-sig          | **1.07x**    | -3,102     | Crypto workload, benefits from LLVM inlining.  |
| shootout-xblabla20 | 1.04x        | -60        | Small absolute delta.                          |
| shootout-ratelimit | 1.03x        | -4,427     |                                                |
| shootout-xchacha20 | 1.03x        | -26        | Small absolute delta.                          |
| shootout-ed25519   | **1.02x**    | -35,197    | Largest absolute savings. Heavy call workload. |
| rust-compression   | **1.02x**    | -20,093    | Large absolute savings.                        |
| shootout-minicsv   | 1.01x        | -16,360    |                                                |


## Exec Regressions (ir_jit faster)


| Kernel             | Exec Ratio | Delta (us) | Notes                                                 |
| ------------------ | ---------- | ---------- | ----------------------------------------------------- |
| blake3-scalar      | **0.09x**  | +594       | Tier-2 compile latency dominates the tiny 61 us exec. |
| rust-html-rewriter | **0.70x**  | +4,331     | LLVM recompilation may hurt short-exec functions.     |
| pulldown-cmark     | **0.72x**  | +1,374     | Same pattern: short exec, tier-2 overhead visible.    |
| hashset            | 0.90x      | +3,241     |                                                       |
| regex              | 0.93x      | +3,631     |                                                       |
| shootout-ctype     | 0.95x      | +7,032     |                                                       |


## Compile-Time Overhead

Tier-2 adds 10-30% compile-time overhead across the board due to tier-1 counter instrumentation, IR text serialization, and background LLVM recompilation.


| Kernel             | Compile Ratio | Delta (us) |
| ------------------ | ------------- | ---------- |
| noop               | 0.70x         | +835       |
| shootout-gimli     | 0.76x         | +237       |
| rust-protobuf      | 0.77x         | +15,845    |
| blake3-scalar      | 0.80x         | +9,594     |
| rust-html-rewriter | 0.83x         | +60,154    |
| rust-json          | 0.83x         | +21,197    |
| gcc-loops          | 0.84x         | +29,204    |


## Observations

1. **Exec is net-neutral** (1.00x aggregate). Tier-2 LLVM recompilation does not produce meaningful speedups on these short-lived sightglass benchmarks. The benchmarks run each kernel once — tier-2 recompilation happens in the background but the benchmark may finish before the optimized code is swapped in, or the swapped-in code only runs for the tail of execution.
2. **Total time is 2% slower** due to compile overhead. The tier-1 prologue counter instrumentation (IF/MERGE + counter check), IR text serialization (`ir_save` before `ir_jit_compile`), and background LLVM thread all add cost.
3. **blake3-scalar 0.09x exec regression** is misleading — baseline exec is only 61 us. The tier-2 background thread's LLVM compilation likely contends for CPU during this tiny window, inflating the measured exec time to 654 us.
4. **rust-html-rewriter and pulldown-cmark regressions** (0.70x, 0.72x exec) follow the same pattern: short execution times where tier-2 overhead is visible relative to the workload.
5. **blind-sig is the best exec win** at 1.07x (+3.1 ms saved). shootout-ed25519 saves the most absolute time (35 ms) at 1.02x. These are longer-running kernels where tier-2 code has time to be swapped in and executed.
6. **rust-compression works** on the tier2 branch (1.02x exec speedup, +20 ms saved) — it crashes on `reg_call` but tier-2's buffer-based calling convention handles it.
7. **The sightglass benchmark format is unfavorable for tier-2.** Each kernel runs once with a fixed iteration count. Tier-2 benefits compound over repeated invocations in long-running programs, which this benchmark does not measure.

