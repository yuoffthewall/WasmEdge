# Register-Based Calling Convention Benchmark

**Date:** 2026-04-09
**Branches:** `reg_call` (register-based, `kRegCallMaxParams = 100`) vs `ir_jit` (baseline, buffer-based)
**Setup:** IR JIT O2, bound-check off, 3 runs per kernel, median taken. Submodules updated per branch.
**Excluded:** spidermonkey, tinygo, rust-compression (crashes on reg_call), richards (no output), shootout-ackermann (high variance)

## Aggregate


| Metric               | ir_jit (us) | reg_call (us) | Speedup |
| -------------------- | ----------- | ------------- | ------- |
| Exec                 | 9,194,428   | 9,183,915     | 1.00x   |
| Total (compile+exec) | 12,849,494  | 12,000,130    | 1.07x   |


Execution time is flat in aggregate. The 7% total-time improvement is driven by compile-time reductions.

## Per-Kernel Results (3-run median, microseconds)


| Kernel              | Base Compile | Reg Compile | Comp Spd | Base Exec | Reg Exec  | Exec Spd | Base Total | Reg Total | Total Spd |
| ------------------- | ------------ | ----------- | -------- | --------- | --------- | -------- | ---------- | --------- | --------- |
| blake3-scalar       | 37,964       | 35,838      | 1.06x    | 61        | 60        | 1.02x    | 54,549     | 52,277    | 1.04x     |
| blind-sig           | 206,641      | 209,252     | 0.99x    | 46,552    | 41,537    | 1.12x    | 333,905    | 329,972   | 1.01x     |
| bz2                 | 97,775       | 97,408      | 1.00x    | 20,277    | 19,930    | 1.02x    | 140,407    | 140,320   | 1.00x     |
| gcc-loops           | 152,203      | 149,958     | 1.01x    | 2,676,964 | 2,676,218 | 1.00x    | 2,881,322  | 2,880,920 | 1.00x     |
| hashset             | 1,018,675    | 246,490     | 4.13x    | 29,157    | 34,811    | 0.84x    | 1,112,499  | 348,380   | 3.19x     |
| noop                | 1,944        | 1,966       | 0.99x    | 2         | 2         | 0.92x    | 2,809      | 2,864     | 0.98x     |
| pulldown-cmark      | 174,989      | 174,394     | 1.00x    | 3,415     | 3,679     | 0.93x    | 223,658    | 224,028   | 1.00x     |
| quicksort           | 16,385       | 15,832      | 1.03x    | 17,629    | 18,014    | 0.98x    | 38,336     | 37,754    | 1.02x     |
| regex               | 310,739      | 298,309     | 1.04x    | 46,854    | 46,788    | 1.00x    | 477,832    | 467,141   | 1.02x     |
| rust-html-rewriter  | 301,283      | 292,244     | 1.03x    | 10,398    | 12,753    | 0.82x    | 419,120    | 415,120   | 1.01x     |
| rust-json           | 103,640      | 98,197      | 1.06x    | 5,266     | 5,635     | 0.93x    | 148,406    | 142,283   | 1.04x     |
| rust-protobuf       | 54,396       | 53,108      | 1.02x    | 3,231     | 3,539     | 0.91x    | 81,646     | 81,041    | 1.01x     |
| shootout-base64     | 21,259       | 20,748      | 1.02x    | 70,492    | 71,590    | 0.98x    | 98,100     | 99,452    | 0.99x     |
| shootout-ctype      | 20,538       | 19,970      | 1.03x    | 140,291   | 132,839   | 1.06x    | 166,916    | 158,965   | 1.05x     |
| shootout-ed25519    | 143,376      | 98,651      | 1.45x    | 1,731,933 | 1,485,257 | 1.17x    | 1,896,525  | 1,617,476 | 1.17x     |
| shootout-fib2       | 16,532       | 16,018      | 1.03x    | 561,523   | 594,979   | 0.94x    | 582,103    | 616,471   | 0.94x     |
| shootout-gimli      | 769          | 795         | 0.97x    | 834       | 823       | 1.01x    | 2,000      | 2,005     | 1.00x     |
| shootout-heapsort   | 5,259        | 4,925       | 1.07x    | 562,575   | 562,344   | 1.00x    | 570,352    | 569,758   | 1.00x     |
| shootout-keccak     | 5,085        | 5,046       | 1.01x    | 2,557     | 2,642     | 0.97x    | 13,007     | 13,320    | 0.98x     |
| shootout-matrix     | 20,748       | 20,311      | 1.02x    | 69,152    | 70,651    | 0.98x    | 96,027     | 97,678    | 0.98x     |
| shootout-memmove    | 21,332       | 21,341      | 1.00x    | 23,688    | 23,557    | 1.01x    | 51,729     | 51,610    | 1.00x     |
| shootout-minicsv    | 2,397        | 2,375       | 1.01x    | 1,179,992 | 1,341,543 | 0.88x    | 1,183,521  | 1,345,060 | 0.88x     |
| shootout-nestedloop | 16,423       | 16,060      | 1.02x    | 1         | 1         | 1.06x    | 20,454     | 20,126    | 1.02x     |
| shootout-random     | 16,754       | 16,176      | 1.04x    | 138,501   | 138,626   | 1.00x    | 159,416    | 158,927   | 1.00x     |
| shootout-ratelimit  | 21,419       | 20,845      | 1.03x    | 173,164   | 201,139   | 0.86x    | 201,071    | 228,336   | 0.88x     |
| shootout-seqhash    | 22,075       | 22,711      | 0.97x    | 1,507,653 | 1,490,682 | 1.01x    | 1,537,228  | 1,521,049 | 1.01x     |
| shootout-sieve      | 16,742       | 16,275      | 1.03x    | 140,237   | 173,047   | 0.81x    | 161,401    | 193,622   | 0.83x     |
| shootout-switch     | 89,838       | 80,429      | 1.12x    | 29,960    | 29,114    | 1.03x    | 137,170    | 127,267   | 1.08x     |
| shootout-xblabla20  | 21,271       | 20,995      | 1.01x    | 1,334     | 1,345     | 0.99x    | 29,050     | 28,990    | 1.00x     |
| shootout-xchacha20  | 21,515       | 20,741      | 1.04x    | 737       | 767       | 0.96x    | 28,934     | 27,918    | 1.04x     |


## Exec Speedups (reg_call faster)


| Kernel           | Exec Speedup | Notes                                                             |
| ---------------- | ------------ | ----------------------------------------------------------------- |
| shootout-ed25519 | **1.17x**    | Largest absolute savings (247 ms). Heavy internal function calls. |
| blind-sig        | **1.12x**    | Crypto workload with frequent calls.                              |
| shootout-ctype   | **1.06x**    | Character-classification loops with calls.                        |
| shootout-switch  | 1.03x        |                                                                   |
| bz2              | 1.02x        |                                                                   |


## Exec Regressions (ir_jit faster)


| Kernel             | Exec Ratio | Delta (us) | Notes                                        |
| ------------------ | ---------- | ---------- | -------------------------------------------- |
| shootout-sieve     | **0.81x**  | +32,810    | Consistent across all 3 runs.                |
| rust-html-rewriter | **0.82x**  | +2,355     | Small absolute delta.                        |
| hashset            | **0.84x**  | +5,654     | Offset by 4.13x compile-time win.            |
| shootout-ratelimit | **0.86x**  | +27,975    | Consistent across all 3 runs.                |
| shootout-minicsv   | **0.88x**  | +161,551   | Largest absolute regression.                 |
| shootout-fib2      | **0.94x**  | +33,456    | Recursive, may stress register save/restore. |


## Compile-Time Speedups


| Kernel           | Compile Speedup | Notes                                                          |
| ---------------- | --------------- | -------------------------------------------------------------- |
| hashset          | **4.13x**       | 1,019 ms down to 246 ms. Dominates the total-time improvement. |
| shootout-ed25519 | **1.45x**       | 143 ms down to 99 ms.                                          |
| shootout-switch  | 1.12x           |                                                                |


## Observations

1. **Exec is net-neutral.** The register ABI helps call-heavy kernels (ed25519 +17%, blind-sig +12%) but hurts others (sieve -19%, ratelimit -14%, minicsv -12%). These cancel out in aggregate.
2. **Total time wins by 7%**, almost entirely from hashset's 4.13x compile-time reduction (772 ms saved). Without hashset the total speedup is ~1.01x.
3. **Consistent regressions** in sieve, ratelimit, and minicsv are real (stable across runs) and worth investigating. Possible causes:
  - Extra register save/restore overhead in the prologue for functions that don't benefit from register passing.
  - Register pressure changes affecting the IR backend's allocation decisions for the function body.
4. **hashset compile-time anomaly**: the 4x compile speedup on `reg_call` is surprisingly large for a calling-convention change. This may indicate the register ABI simplifies the IR graph enough that the backend's register allocator or scheduler converges faster.