# IR JIT Performance Analysis — Sightglass Benchmarks

Date: 2026-04-03

## Benchmark Results (3 Modes x 3 Metrics)

Speedup is relative to LLVM_JIT (1.00x baseline). Higher is better for Inst.Lat and TtV; closer to 1.00x is better for WorkTime.

| Benchmark | Inst.Lat(us) | WorkTime(us) | TtV(us) |
| :--- | :--- | :--- | :--- |
| **blake3-scalar** | IR_JIT: 36476.11 (22.07x)<br>LLVM_JIT: 804919.50 (1.00x)<br>LLVM_AOT: 224.51 (3585.23x) | IR_JIT: 59.84 (0.98x)<br>LLVM_JIT: 58.85 (1.00x)<br>LLVM_AOT: 59.53 (0.99x) | IR_JIT: 52899.93 (15.52x)<br>LLVM_JIT: 820936.05 (1.00x)<br>LLVM_AOT: 1243.97 (659.93x) |
| **blind-sig** | IR_JIT: 219697.54 (16.79x)<br>LLVM_JIT: 3689737.37 (1.00x)<br>LLVM_AOT: 314.25 (11741.41x) | IR_JIT: 45960.21 (0.42x)<br>LLVM_JIT: 19443.72 (1.00x)<br>LLVM_AOT: 19240.84 (1.01x) | IR_JIT: 354487.43 (10.68x)<br>LLVM_JIT: 3787313.87 (1.00x)<br>LLVM_AOT: 21438.24 (176.66x) |
| **bz2** | IR_JIT: 98209.28 (13.38x)<br>LLVM_JIT: 1314493.04 (1.00x)<br>LLVM_AOT: 142.78 (9206.42x) | IR_JIT: 20293.15 (0.91x)<br>LLVM_JIT: 18485.22 (1.00x)<br>LLVM_AOT: 18075.06 (1.02x) | IR_JIT: 141122.87 (9.60x)<br>LLVM_JIT: 1355460.65 (1.00x)<br>LLVM_AOT: 19004.25 (71.32x) |
| **gcc-loops** | IR_JIT: 151939.58 (21.31x)<br>LLVM_JIT: 3238074.98 (1.00x)<br>LLVM_AOT: 594.48 (5446.90x) | IR_JIT: 2720061.93 (0.84x)<br>LLVM_JIT: 2295360.78 (1.00x)<br>LLVM_AOT: 2281642.78 (1.01x) | IR_JIT: 2927378.53 (1.91x)<br>LLVM_JIT: 5594680.75 (1.00x)<br>LLVM_AOT: 2285231.63 (2.45x) |
| **hashset** | IR_JIT: 831975.89 (3.35x)<br>LLVM_JIT: 2787841.30 (1.00x)<br>LLVM_AOT: 128.65 (21669.97x) | IR_JIT: 29265.13 (0.91x)<br>LLVM_JIT: 26681.42 (1.00x)<br>LLVM_AOT: 26452.27 (1.01x) | IR_JIT: 927470.67 (3.10x)<br>LLVM_JIT: 2876432.53 (1.00x)<br>LLVM_AOT: 27469.75 (104.71x) |
| **noop** | IR_JIT: 2003.07 (23.46x)<br>LLVM_JIT: 46991.29 (1.00x)<br>LLVM_AOT: 89.73 (523.70x) | IR_JIT: 1.77 (0.86x)<br>LLVM_JIT: 1.53 (1.00x)<br>LLVM_AOT: 1.80 (0.85x) | IR_JIT: 2893.04 (16.53x)<br>LLVM_JIT: 47821.75 (1.00x)<br>LLVM_AOT: 426.90 (112.02x) |
| **pulldown-cmark** | IR_JIT: 177215.64 (13.30x)<br>LLVM_JIT: 2357727.79 (1.00x)<br>LLVM_AOT: 316.14 (7457.86x) | IR_JIT: 3464.73 (0.93x)<br>LLVM_JIT: 3219.20 (1.00x)<br>LLVM_AOT: 3379.05 (0.95x) | IR_JIT: 226420.60 (10.63x)<br>LLVM_JIT: 2406282.63 (1.00x)<br>LLVM_AOT: 5641.85 (426.51x) |
| **quicksort** | IR_JIT: 16707.66 (13.80x)<br>LLVM_JIT: 230546.42 (1.00x)<br>LLVM_AOT: 97.52 (2364.09x) | IR_JIT: 18069.92 (1.01x)<br>LLVM_JIT: 18295.69 (1.00x)<br>LLVM_AOT: 18304.57 (1.00x) | IR_JIT: 38789.42 (6.51x)<br>LLVM_JIT: 252601.22 (1.00x)<br>LLVM_AOT: 18767.42 (13.46x) |
| **regex** | IR_JIT: 356494.41 (16.35x)<br>LLVM_JIT: 5830105.40 (1.00x)<br>LLVM_AOT: 594.31 (9809.87x) | IR_JIT: 71493.42 (0.60x)<br>LLVM_JIT: 43098.43 (1.00x)<br>LLVM_AOT: 43950.35 (0.98x) | IR_JIT: 555479.94 (10.78x)<br>LLVM_JIT: 5988243.92 (1.00x)<br>LLVM_AOT: 54312.03 (110.26x) |
| **richards** | IR_JIT: 4439.89 (17.35x)<br>LLVM_JIT: 77024.32 (1.00x)<br>LLVM_AOT: 77.69 (991.43x) | IR_JIT: 22942901.09 (1.06x)<br>LLVM_JIT: 24296631.75 (1.00x)<br>LLVM_AOT: 24304367.92 (1.00x) | IR_JIT: 22949736.51 (1.06x)<br>LLVM_JIT: 24375961.15 (1.00x)<br>LLVM_AOT: 24304740.68 (1.00x) |
| **rust-compression** | IR_JIT: 498201.41 (14.30x)<br>LLVM_JIT: 7125180.86 (1.00x)<br>LLVM_AOT: 617.84 (11532.40x) | IR_JIT: 1018455.94 (0.71x)<br>LLVM_JIT: 722525.32 (1.00x)<br>LLVM_AOT: 771722.28 (0.94x) | IR_JIT: 1641771.84 (4.85x)<br>LLVM_JIT: 7969730.96 (1.00x)<br>LLVM_AOT: 776078.27 (10.27x) |
| **rust-html-rewriter** | IR_JIT: 293777.00 (19.17x)<br>LLVM_JIT: 5630788.60 (1.00x)<br>LLVM_AOT: 594.41 (9472.90x) | IR_JIT: 9760.29 (1.01x)<br>LLVM_JIT: 9839.86 (1.00x)<br>LLVM_AOT: 9539.61 (1.03x) | IR_JIT: 414949.85 (13.85x)<br>LLVM_JIT: 5745407.71 (1.00x)<br>LLVM_AOT: 12812.18 (448.43x) |
| **rust-json** | IR_JIT: 109653.36 (19.35x)<br>LLVM_JIT: 2121388.47 (1.00x)<br>LLVM_AOT: 302.49 (7013.09x) | IR_JIT: 6662.68 (0.73x)<br>LLVM_JIT: 4843.29 (1.00x)<br>LLVM_AOT: 4848.10 (1.00x) | IR_JIT: 157552.03 (13.74x)<br>LLVM_JIT: 2164627.16 (1.00x)<br>LLVM_AOT: 7726.48 (280.16x) |
| **rust-protobuf** | IR_JIT: 53429.75 (21.65x)<br>LLVM_JIT: 1156981.50 (1.00x)<br>LLVM_AOT: 249.12 (4644.27x) | IR_JIT: 3038.43 (0.97x)<br>LLVM_JIT: 2932.21 (1.00x)<br>LLVM_AOT: 3029.03 (0.97x) | IR_JIT: 81338.45 (14.55x)<br>LLVM_JIT: 1183224.16 (1.00x)<br>LLVM_AOT: 5780.59 (204.69x) |
| **shootout-ackermann** | IR_JIT: 21753.61 (15.52x)<br>LLVM_JIT: 337620.70 (1.00x)<br>LLVM_AOT: 110.31 (3060.65x) | IR_JIT: 730.52 (0.53x)<br>LLVM_JIT: 388.87 (1.00x)<br>LLVM_AOT: 381.61 (1.02x) | IR_JIT: 30120.38 (11.43x)<br>LLVM_JIT: 344316.17 (1.00x)<br>LLVM_AOT: 910.68 (378.09x) |
| **shootout-base64** | IR_JIT: 20664.51 (15.10x)<br>LLVM_JIT: 312014.80 (1.00x)<br>LLVM_AOT: 93.06 (3352.83x) | IR_JIT: 70729.81 (1.21x)<br>LLVM_JIT: 85779.90 (1.00x)<br>LLVM_AOT: 57298.56 (1.50x) | IR_JIT: 97799.94 (4.13x)<br>LLVM_JIT: 403913.57 (1.00x)<br>LLVM_AOT: 57750.83 (6.99x) |
| **shootout-ctype** | IR_JIT: 20398.76 (14.37x)<br>LLVM_JIT: 293228.98 (1.00x)<br>LLVM_AOT: 98.51 (2976.64x) | IR_JIT: 140252.23 (0.60x)<br>LLVM_JIT: 84142.93 (1.00x)<br>LLVM_AOT: 82917.52 (1.01x) | IR_JIT: 167245.39 (2.29x)<br>LLVM_JIT: 383206.63 (1.00x)<br>LLVM_AOT: 83385.25 (4.60x) |
| **shootout-ed25519** | IR_JIT: 130335.75 (19.92x)<br>LLVM_JIT: 2596864.62 (1.00x)<br>LLVM_AOT: 77.21 (33633.79x) | IR_JIT: 1718026.30 (0.62x)<br>LLVM_JIT: 1057357.22 (1.00x)<br>LLVM_AOT: 1067294.92 (0.99x) | IR_JIT: 1869934.64 (1.96x)<br>LLVM_JIT: 3674408.32 (1.00x)<br>LLVM_AOT: 1067747.68 (3.44x) |
| **shootout-fib2** | IR_JIT: 16346.51 (12.58x)<br>LLVM_JIT: 205583.46 (1.00x)<br>LLVM_AOT: 90.58 (2269.63x) | IR_JIT: 658161.67 (0.71x)<br>LLVM_JIT: 464733.90 (1.00x)<br>LLVM_AOT: 484923.65 (0.96x) | IR_JIT: 678789.63 (0.99x)<br>LLVM_JIT: 674273.21 (1.00x)<br>LLVM_AOT: 485371.80 (1.39x) |
| **shootout-gimli** | IR_JIT: 768.19 (18.93x)<br>LLVM_JIT: 14541.99 (1.00x)<br>LLVM_AOT: 69.85 (208.19x) | IR_JIT: 858.29 (0.90x)<br>LLVM_JIT: 775.92 (1.00x)<br>LLVM_AOT: 773.95 (1.00x) | IR_JIT: 2031.59 (7.70x)<br>LLVM_JIT: 15636.30 (1.00x)<br>LLVM_AOT: 1040.21 (15.03x) |
| **shootout-heapsort** | IR_JIT: 4817.04 (18.36x)<br>LLVM_JIT: 88458.30 (1.00x)<br>LLVM_AOT: 76.03 (1163.47x) | IR_JIT: 564881.09 (0.96x)<br>LLVM_JIT: 545067.29 (1.00x)<br>LLVM_AOT: 516212.42 (1.06x) | IR_JIT: 572204.08 (1.11x)<br>LLVM_JIT: 635934.06 (1.00x)<br>LLVM_AOT: 516567.80 (1.23x) |
| **shootout-keccak** | IR_JIT: 5078.94 (75.93x)<br>LLVM_JIT: 385664.09 (1.00x)<br>LLVM_AOT: 71.81 (5370.62x) | IR_JIT: 2560.86 (0.84x)<br>LLVM_JIT: 2140.95 (1.00x)<br>LLVM_AOT: 2143.25 (1.00x) | IR_JIT: 13024.61 (30.15x)<br>LLVM_JIT: 392672.88 (1.00x)<br>LLVM_AOT: 2480.02 (158.33x) |
| **shootout-matrix** | IR_JIT: 20545.04 (15.50x)<br>LLVM_JIT: 318360.60 (1.00x)<br>LLVM_AOT: 92.70 (3434.31x) | IR_JIT: 69257.02 (0.83x)<br>LLVM_JIT: 57729.79 (1.00x)<br>LLVM_AOT: 59392.55 (0.97x) | IR_JIT: 96200.74 (3.97x)<br>LLVM_JIT: 382054.93 (1.00x)<br>LLVM_AOT: 59848.35 (6.38x) |
| **shootout-memmove** | IR_JIT: 21375.72 (14.49x)<br>LLVM_JIT: 309797.56 (1.00x)<br>LLVM_AOT: 101.75 (3044.69x) | IR_JIT: 23627.25 (1.04x)<br>LLVM_JIT: 24629.10 (1.00x)<br>LLVM_AOT: 25712.36 (0.96x) | IR_JIT: 52079.40 (6.55x)<br>LLVM_JIT: 341306.75 (1.00x)<br>LLVM_AOT: 26191.97 (13.03x) |
| **shootout-minicsv** | IR_JIT: 2447.86 (21.49x)<br>LLVM_JIT: 52610.54 (1.00x)<br>LLVM_AOT: 70.09 (750.61x) | IR_JIT: 1231274.20 (0.96x)<br>LLVM_JIT: 1179429.17 (1.00x)<br>LLVM_AOT: 1178212.38 (1.00x) | IR_JIT: 1234910.34 (1.00x)<br>LLVM_JIT: 1233148.27 (1.00x)<br>LLVM_AOT: 1178523.37 (1.05x) |
| **shootout-nestedloop** | IR_JIT: 16612.76 (13.52x)<br>LLVM_JIT: 224680.09 (1.00x)<br>LLVM_AOT: 98.34 (2284.73x) | IR_JIT: 1.30 (1.08x)<br>LLVM_JIT: 1.41 (1.00x)<br>LLVM_AOT: 1.21 (1.17x) | IR_JIT: 20694.92 (11.04x)<br>LLVM_JIT: 228514.68 (1.00x)<br>LLVM_AOT: 440.60 (518.64x) |
| **shootout-random** | IR_JIT: 16473.23 (13.33x)<br>LLVM_JIT: 219514.37 (1.00x)<br>LLVM_AOT: 101.30 (2166.97x) | IR_JIT: 144064.14 (0.61x)<br>LLVM_JIT: 87570.12 (1.00x)<br>LLVM_AOT: 87471.58 (1.00x) | IR_JIT: 164749.14 (1.89x)<br>LLVM_JIT: 311388.11 (1.00x)<br>LLVM_AOT: 87933.70 (3.54x) |
| **shootout-ratelimit** | IR_JIT: 20914.71 (16.59x)<br>LLVM_JIT: 346997.10 (1.00x)<br>LLVM_AOT: 91.86 (3777.46x) | IR_JIT: 169991.36 (1.02x)<br>LLVM_JIT: 174213.26 (1.00x)<br>LLVM_AOT: 191322.82 (0.91x) | IR_JIT: 198054.11 (2.67x)<br>LLVM_JIT: 529620.50 (1.00x)<br>LLVM_AOT: 191874.48 (2.76x) |
| **shootout-seqhash** | IR_JIT: 23121.42 (14.30x)<br>LLVM_JIT: 330634.73 (1.00x)<br>LLVM_AOT: 97.28 (3398.79x) | IR_JIT: 1497492.73 (1.06x)<br>LLVM_JIT: 1585706.40 (1.00x)<br>LLVM_AOT: 1567113.75 (1.01x) | IR_JIT: 1528242.53 (1.26x)<br>LLVM_JIT: 1923535.70 (1.00x)<br>LLVM_AOT: 1567824.16 (1.23x) |
| **shootout-sieve** | IR_JIT: 16324.20 (14.79x)<br>LLVM_JIT: 241463.28 (1.00x)<br>LLVM_AOT: 94.97 (2542.52x) | IR_JIT: 176229.01 (0.69x)<br>LLVM_JIT: 121814.75 (1.00x)<br>LLVM_AOT: 144351.40 (0.84x) | IR_JIT: 196710.52 (1.87x)<br>LLVM_JIT: 368463.08 (1.00x)<br>LLVM_AOT: 144830.07 (2.54x) |
| **shootout-switch** | IR_JIT: 88371.03 (31.19x)<br>LLVM_JIT: 2756137.70 (1.00x)<br>LLVM_AOT: 108.54 (25392.83x) | IR_JIT: 29279.08 (1.04x)<br>LLVM_JIT: 30428.56 (1.00x)<br>LLVM_AOT: 35645.07 (0.85x) | IR_JIT: 135683.08 (20.66x)<br>LLVM_JIT: 2803081.93 (1.00x)<br>LLVM_AOT: 36262.07 (77.30x) |
| **shootout-xblabla20** | IR_JIT: 21807.47 (15.06x)<br>LLVM_JIT: 328403.67 (1.00x)<br>LLVM_AOT: 88.01 (3731.44x) | IR_JIT: 1309.49 (1.04x)<br>LLVM_JIT: 1366.77 (1.00x)<br>LLVM_AOT: 1515.84 (0.90x) | IR_JIT: 30547.89 (11.02x)<br>LLVM_JIT: 336516.67 (1.00x)<br>LLVM_AOT: 1921.55 (175.13x) |
| **shootout-xchacha20** | IR_JIT: 20817.95 (14.05x)<br>LLVM_JIT: 292566.41 (1.00x)<br>LLVM_AOT: 88.76 (3296.15x) | IR_JIT: 731.91 (1.02x)<br>LLVM_JIT: 744.16 (1.00x)<br>LLVM_AOT: 744.43 (1.00x) | IR_JIT: 27997.92 (10.70x)<br>LLVM_JIT: 299472.89 (1.00x)<br>LLVM_AOT: 1163.42 (257.41x) |

## Kernels with WorkTime < 0.8x (IR_JIT slower than LLVM_JIT)

| Kernel | IR_JIT WorkTime(us) | LLVM_JIT WorkTime(us) | Ratio |
| :--- | ---: | ---: | ---: |
| **blind-sig** | 45960 | 19444 | 0.42x |
| **shootout-ackermann** | 731 | 389 | 0.53x |
| **shootout-ctype** | 140252 | 84143 | 0.60x |
| **regex** | 71493 | 43098 | 0.60x |
| **shootout-random** | 144064 | 87570 | 0.61x |
| **shootout-ed25519** | 1718026 | 1057357 | 0.62x |
| **shootout-sieve** | 176229 | 121815 | 0.69x |
| **shootout-fib2** | 658162 | 464734 | 0.71x |
| **rust-compression** | 1018456 | 722525 | 0.71x |
| **rust-json** | 6663 | 4843 | 0.73x |

---

## Bottleneck Analysis

### Bottleneck 1: Indirect Calls for All Wasm Function Calls (Biggest Impact)

**Affected kernels:** shootout-ackermann (0.53x), blind-sig (0.42x), shootout-fib2 (0.71x), shootout-ed25519 (0.62x)

Every Wasm `call` instruction -- even to a known local function index -- is lowered to an
indirect call through FuncTable (`ir_builder.cpp:3051-3054`):

```
LOAD FuncTable[funcIdx]   // load function pointer at runtime
PROTO ...                 // cast to calling convention
CALL/2 (indirect)         // indirect call through loaded pointer
```

The ackermann IR dump (`wasmedge_ir_052_after.ir`) confirms both recursive self-calls are
indirect:

```
uintptr_t d_11, l_11 = LOAD(l_9, d_10);              // load FuncTable[idx]
uintptr_t d_12 = PROTO(d_11, ...);
int64_t d_13, l_13 = CALL/2(l_11, d_12, d_2, d_8);   // indirect call
```

**LLVM JIT by contrast** emits direct `call` instructions between Wasm functions
(`compiler.cpp:4230`), and LLVM's inliner can then inline small callees entirely. For
ackermann (deep recursion, millions of calls), every indirect call costs ~5-10 extra cycles
for branch prediction vs a direct `call rel32`.

**Suggested improvement -- direct call patching:**

After all functions in a module are compiled, their native addresses are known. A
post-compilation pass could:

1. Record each call site's `FuncTable[idx]` during IR building (funcIdx + call-site offset).
2. After all functions compile, patch the indirect `LOAD+CALL` into a direct `CALL` with
   the known address.
3. For the IR framework specifically: emit a placeholder `CALL` with a constant function
   address (initially a stub), then backpatch after all functions are JIT'd.

Alternatively, compile all functions to IR in a single `ir_ctx` so the IR framework can see
cross-function references and potentially inline. This is a bigger change but matches what
LLVM does.

### Bottleneck 2: Memory-Based Argument Passing (Major Impact)

**Affected kernels:** All call-heavy kernels (compounds with Bottleneck 1)

Every call marshals arguments through a memory buffer (`ir_builder.cpp:2934-2946`):

```c
// Caller side: store args to stack buffer
for (uint32_t i = 0; i < NumArgs; ++i) {
    ir_STORE(CalleeArgs + i*8, WasmArgs[i]);   // STORE to memory
}
// ... CALL(env, buffer_ptr) ...

// Callee side: reload args from buffer (ir_builder.cpp:381-384)
for (uint32_t i = 0; i < ParamTypes.size(); ++i) {
    Locals[i] = ir_LOAD(irType, ArgsPtr + i*8);  // LOAD from memory
}
```

For ackermann with 2 i32 args, each call does 2 stores + 2 loads through memory that a
register-based convention would eliminate. At millions of recursive calls, that is millions
of unnecessary store-load pairs.

**LLVM JIT by contrast** passes Wasm arguments directly as LLVM function parameters, which
map to registers via the platform ABI.

**Suggested improvement -- register-based calling convention for small functions:**

For functions with <= 4 integer/float params (covers the vast majority of Wasm functions):

1. Emit the callee with a prototype that takes params as direct IR parameters:
   `func(env, arg0, arg1, ...) : retType`
2. Emit the caller with matching direct argument passing.
3. Fall back to the current buffer convention for functions with > 4 params.

This requires knowing the callee's signature at IR build time (which we already do --
`TargetFuncType` is available at `ir_builder.cpp:2920`). The `invoke()` entry point in
`ir_jit_engine.cpp` also needs to marshal to the new convention, but this is
straightforward for a known param count.

### Bottleneck 3: No Cross-Function Inlining (Moderate Impact)

**Affected kernels:** shootout-random (0.61x), shootout-ctype (0.60x), shootout-sieve (0.69x), rust-json (0.73x), rust-compression (0.71x)

Each Wasm function is compiled as an independent `ir_ctx`. The IR framework's
`IR_OPT_INLINE` flag only applies to inlining within a single function's IR graph (e.g.
inlining helper calls). Small hot functions called from inner loops (like `random_next` in
shootout-random) can never be inlined into their callers.

For shootout-random: the inner loop calls a small RNG function millions of times. Each call
pays the full indirect-call + arg-marshal overhead instead of being a few arithmetic
instructions inlined into the loop body.

**Suggested improvement -- whole-module IR compilation:**

Compile all (or a group of hot) functions into a single `ir_ctx`:

1. During module instantiation, build IR for all functions into one context.
2. Wasm `call` to a local function becomes an internal IR `CALL` to a known target.
3. The IR framework's SCCP + GCM + inline passes can then optimize across functions.

This is a significant architectural change but would close the gap with LLVM on all three
bottlenecks simultaneously.

### Bottleneck 4: No Loop Unrolling / Vectorization (Minor Impact)

**Affected kernels:** shootout-sieve (0.69x), shootout-ctype (0.60x), shootout-matrix (0.83x), shootout-keccak (0.84x)

The IR framework at O2 does SCCP, GCM, and register allocation, but does not unroll loops
or vectorize. For tight integer loops (sieve's inner marking loop, ctype's character
scanning), LLVM's loop unroller produces 2-4x less loop overhead.

This is the lowest-priority item because the IR framework (`thirdparty/ir` from dstogov)
does not currently implement loop unrolling. Adding it would mean modifying the third-party
library.

---

## Priority-Ordered Improvement Roadmap

| Priority | Change | Effort | Expected Impact | Kernels Fixed |
|:---|:---|:---|:---|:---|
| **1** | Direct call patching (post-compile backpatch) | Medium | Fixes indirect-call overhead for all local calls | ackermann, fib2, blind-sig, ed25519 |
| **2** | Register-based calling convention (<= 4 params) | Medium | Eliminates store/load pairs per call | All 10 kernels |
| **3** | Whole-module IR compilation | Large | Enables cross-function inlining + direct calls natively | random, ctype, sieve, rust-json, rust-compression |
| **4** | Loop unrolling in IR framework | Large | Reduces loop overhead in tight loops | sieve, ctype, matrix, keccak |

Items 1 and 2 are independent and can be done in parallel. Together they should bring most
of the call-heavy kernels (ackermann, fib2, blind-sig) to within 0.85-0.95x of LLVM. Item
3 is the "silver bullet" that addresses all bottlenecks but requires the most architectural
work.