# Direct-Call Benchmark

**Date:** 2026-04-09
**Branches:** `direct_call` vs `ir_jit` (baseline)
**Setup:** IR JIT O2, bound-check off, 3 runs per kernel, median taken. Submodules updated per branch.
**Excluded:** spidermonkey, tinygo, rust-compression, richards (no output), shootout-ackermann (high variance)

## Aggregate


| Metric               | ir_jit (us) | direct_call (us) | Speedup |
| -------------------- | ----------- | ---------------- | ------- |
| Exec                 | 9,213,072   | 9,591,199        | 0.96x   |
| Total (compile+exec) | 12,826,210  | 12,505,011       | 1.03x   |


Execution is **4% slower** in aggregate on `direct_call`. Total time is 3% faster, driven by hashset's compile-time reduction.

## Per-Kernel Results (3-run median, microseconds)


| Kernel              | Base Compile | DC Compile | Comp Spd | Base Exec | DC Exec   | Exec Spd | Base Total | DC Total  | Total Spd |
| ------------------- | ------------ | ---------- | -------- | --------- | --------- | -------- | ---------- | --------- | --------- |
| blake3-scalar       | 36,835       | 37,721     | 0.98x    | 60        | 60        | 1.00x    | 53,097     | 53,840    | 0.99x     |
| blind-sig           | 206,287      | 208,329    | 0.99x    | 46,443    | 47,558    | 0.98x    | 332,910    | 338,262   | 0.98x     |
| bz2                 | 96,518       | 95,757     | 1.01x    | 20,083    | 20,638    | 0.97x    | 138,635    | 138,776   | 1.00x     |
| gcc-loops           | 150,187      | 149,915    | 1.00x    | 2,685,644 | 2,692,569 | 1.00x    | 2,890,822  | 2,898,145 | 1.00x     |
| hashset             | 1,001,693    | 341,619    | 2.93x    | 29,281    | 29,100    | 1.01x    | 1,095,864  | 436,018   | 2.51x     |
| noop                | 1,970        | 2,085      | 0.95x    | 2         | 2         | 0.93x    | 2,831      | 2,955     | 0.96x     |
| pulldown-cmark      | 177,410      | 171,642    | 1.03x    | 3,446     | 3,470     | 0.99x    | 226,975    | 220,631   | 1.03x     |
| quicksort           | 16,238       | 16,152     | 1.01x    | 17,955    | 18,369    | 0.98x    | 38,070     | 38,471    | 0.99x     |
| regex               | 302,108      | 289,959    | 1.04x    | 47,316    | 46,137    | 1.03x    | 466,582    | 454,135   | 1.03x     |
| rust-html-rewriter  | 301,851      | 288,742    | 1.05x    | 10,419    | 10,128    | 1.03x    | 419,787    | 408,172   | 1.03x     |
| rust-json           | 102,965      | 100,961    | 1.02x    | 5,217     | 5,289     | 0.99x    | 146,701    | 144,444   | 1.02x     |
| rust-protobuf       | 54,578       | 51,994     | 1.05x    | 3,184     | 3,204     | 0.99x    | 81,652     | 79,067    | 1.03x     |
| shootout-base64     | 21,364       | 21,022     | 1.02x    | 70,203    | 70,829    | 0.99x    | 98,087     | 98,079    | 1.00x     |
| shootout-ctype      | 20,418       | 20,425     | 1.00x    | 139,499   | 144,430   | 0.97x    | 165,945    | 171,298   | 0.97x     |
| shootout-ed25519    | 144,457      | 133,267    | 1.08x    | 1,718,176 | 1,794,706 | 0.96x    | 1,880,207  | 1,950,621 | 0.96x     |
| shootout-fib2       | 16,542       | 16,151     | 1.02x    | 563,317   | 795,445   | 0.71x    | 583,951    | 815,724   | 0.72x     |
| shootout-gimli      | 789          | 801        | 0.98x    | 824       | 821       | 1.00x    | 2,026      | 2,024     | 1.00x     |
| shootout-heapsort   | 4,917        | 4,961      | 0.99x    | 572,955   | 572,378   | 1.00x    | 580,286    | 579,822   | 1.00x     |
| shootout-keccak     | 5,080        | 5,053      | 1.01x    | 2,557     | 2,561     | 1.00x    | 13,061     | 12,947    | 1.01x     |
| shootout-matrix     | 20,827       | 20,454     | 1.02x    | 69,648    | 73,165    | 0.95x    | 96,792     | 99,923    | 0.97x     |
| shootout-memmove    | 21,502       | 20,939     | 1.03x    | 23,802    | 23,688    | 1.00x    | 52,341     | 51,304    | 1.02x     |
| shootout-minicsv    | 2,388        | 2,482      | 0.96x    | 1,201,513 | 1,206,243 | 1.00x    | 1,205,043  | 1,209,950 | 1.00x     |
| shootout-nestedloop | 16,603       | 16,111     | 1.03x    | 1         | 1         | 1.10x    | 20,672     | 20,340    | 1.02x     |
| shootout-random     | 16,563       | 16,283     | 1.02x    | 139,323   | 141,071   | 0.99x    | 160,034    | 161,400   | 0.99x     |
| shootout-ratelimit  | 21,216       | 21,017     | 1.01x    | 170,581   | 174,457   | 0.98x    | 198,100    | 202,100   | 0.98x     |
| shootout-seqhash    | 22,186       | 21,624     | 1.03x    | 1,500,678 | 1,506,377 | 1.00x    | 1,530,316  | 1,535,055 | 1.00x     |
| shootout-sieve      | 16,467       | 16,484     | 1.00x    | 139,293   | 175,507   | 0.79x    | 159,833    | 195,697   | 0.82x     |
| shootout-switch     | 80,940       | 80,694     | 1.00x    | 29,552    | 30,862    | 0.96x    | 128,032    | 129,229   | 0.99x     |
| shootout-xblabla20  | 21,376       | 20,669     | 1.03x    | 1,357     | 1,341     | 1.01x    | 29,213     | 28,447    | 1.03x     |
| shootout-xchacha20  | 21,179       | 20,855     | 1.02x    | 749       | 793       | 0.94x    | 28,340     | 28,136    | 1.01x     |


## Exec Speedups (direct_call faster)


| Kernel             | Exec Speedup | Notes |
| ------------------ | ------------ | ----- |
| regex              | 1.03x        |       |
| rust-html-rewriter | 1.03x        |       |
| shootout-xblabla20 | 1.01x        |       |
| hashset            | 1.01x        |       |


Very few exec wins; the largest meaningful ones are regex and rust-html-rewriter at 3%.

## Exec Regressions (ir_jit faster)


| Kernel             | Exec Ratio | Delta (us) | Notes                                            |
| ------------------ | ---------- | ---------- | ------------------------------------------------ |
| shootout-fib2      | **0.71x**  | +232,128   | Largest regression by far. Recursive call-heavy. |
| shootout-sieve     | **0.79x**  | +36,214    | Consistent across all 3 runs.                    |
| shootout-matrix    | **0.95x**  | +3,517     |                                                  |
| shootout-ed25519   | **0.96x**  | +76,530    | Large absolute delta despite small ratio.        |
| shootout-switch    | 0.96x      | +1,310     |                                                  |
| shootout-ctype     | 0.97x      | +4,931     |                                                  |
| bz2                | 0.97x      | +555       |                                                  |
| blind-sig          | 0.98x      | +1,115     |                                                  |
| shootout-ratelimit | 0.98x      | +3,876     |                                                  |


## Compile-Time Speedups


| Kernel             | Compile Speedup | Notes                                              |
| ------------------ | --------------- | -------------------------------------------------- |
| hashset            | **2.93x**       | 1,002 ms down to 342 ms. Dominates total-time win. |
| shootout-ed25519   | 1.08x           | 144 ms down to 133 ms.                             |
| rust-protobuf      | 1.05x           |                                                    |
| rust-html-rewriter | 1.05x           |                                                    |
| regex              | 1.04x           |                                                    |


## Observations

1. **Exec is 4% slower overall.** `direct_call` regresses on more kernels than it helps. The two biggest losers are shootout-fib2 (-29%, +232 ms) and shootout-sieve (-21%, +36 ms).
2. **Total time is 3% faster**, again driven by hashset's compile-time reduction (2.93x). Without hashset the total speedup would be ~1.00x.
3. **shootout-fib2 regression is severe** (0.71x). This kernel is dominated by recursive function calls. The direct-call path may be introducing overhead in the call sequence that compounds over deep recursion.
4. **shootout-sieve regression** (0.79x) is consistent across runs, matching the same pattern seen in the `reg_call` benchmark. This suggests the regression is inherent to the direct-call mechanism shared by both branches, not specific to register-based calling.
5. **No meaningful exec wins.** The best exec speedups (regex 1.03x, rust-html-rewriter 1.03x) are marginal. Unlike `reg_call`, `direct_call` does not produce the strong wins on call-heavy kernels (ed25519, blind-sig) that the register ABI achieves.

