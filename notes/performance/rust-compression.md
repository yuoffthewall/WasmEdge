# rust-compression IR JIT Performance Analysis

**Date:** 2026-04-09
**Kernel:** `rust-compression.wasm` (1.3 MB, 637 defined functions, 12 imports)
**Opt level:** O2 (`WASMEDGE_IR_JIT_OPT_LEVEL=2`)

## Benchmark Results

| Metric | IR JIT O2 | LLVM JIT | AOT | IR JIT / AOT |
|--------|-----------|----------|-----|---------------|
| Inst. Latency (us) | 493,188 | 6,682,550 | 602 | 820x |
| WorkTime (us) | 1,022,204 | 729,281 | 712,648 | 1.43x |
| TtV (us) | 1,640,119 | 7,532,240 | 716,834 | 2.29x |

IR JIT compiles 820x faster than LLVM JIT (493ms vs 6.7s), but generated code
runs 43% slower than AOT/LLVM-JIT at runtime.  Both dimensions matter for
different use-cases; this note dissects each.

## Module Profile

```
Total defined functions:     637  (635 compiled, 2 unreachable trap stubs)
Compile failures:            0
Import functions:            12
Wasm code section:           533 KB
Top function sizes (wasm bytecode):
  #1  45,410 B   (21,440 instrs)  -- func 34
  #2  29,077 B   (14,285 instrs)  -- func 33
  #3  24,063 B   (10,118 instrs)  -- func 37
  #4  11,949 B   ( 4,857 instrs)  -- func 185
  #5  10,858 B   ( 5,310 instrs)  -- func 353
  #6  10,242 B   ( 4,352 instrs)  -- func 53
Size distribution: <100B: 185, 100-1K: 339, 1K-10K: 107, >10K: 6
```

---

## Part A: Instantiation Latency (493 ms)

### Time Breakdown

| Phase | Time (us) | % |
|-------|-----------|---|
| Wasm-to-IR build | 251,263 | 52% |
| IR-to-native compile (ir_jit_compile) | 230,364 | 48% |
| **Total** | **481,627** | 100% |

(Gap to measured 493 ms is module loading + validation + env setup.)

At O0 the same module compiles in 324 ms (build dominates; no optimiser passes),
so the optimiser adds ~170 ms (+35%).  O0 WorkTime is 4,137 ms -- 4x slower
than O2 -- so the optimiser pays for itself in a single invocation.

### Top 20 Slowest Functions

```
func  34   instrs=21440   build= 62,257us  compile= 21,103us  total= 83,360us
func  37   instrs=10118   build= 19,585us  compile= 34,101us  total= 53,686us
func  33   instrs=14285   build= 19,062us  compile= 15,777us  total= 34,839us
func 185   instrs= 4857   build=  5,177us  compile= 19,780us  total= 24,957us
func 353   instrs= 5310   build=  7,175us  compile=  4,945us  total= 12,120us
func  53   instrs= 4352   build=  5,185us  compile=  5,683us  total= 10,868us
func  60   instrs= 4291   build=  6,094us  compile=  3,426us  total=  9,519us
func  51   instrs= 3277   build=  4,330us  compile=  3,247us  total=  7,578us
func 347   instrs= 3845   build=  4,357us  compile=  3,013us  total=  7,370us
func 101   instrs= 6373   build=  3,971us  compile=  2,973us  total=  6,944us
func 567   instrs= 3188   build=  3,172us  compile=  3,579us  total=  6,751us
func  67   instrs= 2550   build=  4,691us  compile=  1,367us  total=  6,058us
func  73   instrs= 3232   build=  4,237us  compile=  1,805us  total=  6,042us
func 349   instrs= 2875   build=  3,136us  compile=  2,441us  total=  5,577us
func  87   instrs= 2117   build=  3,398us  compile=  1,832us  total=  5,230us
func  41   instrs= 2422   build=  2,921us  compile=  2,037us  total=  4,958us
func 568   instrs= 2703   build=  2,592us  compile=  2,259us  total=  4,851us
func  74   instrs= 2489   build=  2,521us  compile=  1,585us  total=  4,106us
func  72   instrs= 2202   build=  2,656us  compile=  1,404us  total=  4,060us
func  83   instrs= 1531   build=  2,091us  compile=  1,346us  total=  3,436us
```

The top 3 functions alone account for 171 ms (35% of total compilation time).

### Compilation Bottlenecks

**1. Wasm-to-IR build phase (52% of time)**

The build phase is the single largest cost.  Root causes:

- **Instruction vector deep-copy** (`module.cpp:259`): every function's
  instructions are copied from the AST InstrView into a fresh
  `std::vector<AST::Instruction>`.  For func 34 (21,440 instrs), this is a
  non-trivial allocation+copy on every compilation.

- **IR graph inflation**: each wasm memory load/store generates 3 IR nodes
  (`ZEXT` + `ADD` + `LOAD`/`STORE`).  Func 34 alone produces 835 KB of IR text.
  Module-wide totals from the before-optimisation IR:
  - 15,774 `ZEXT` nodes (address extensions)
  - 2,682 `CALL` nodes (function calls with arg marshalling)
  - 2,742 `PHI` nodes (in func 34 alone)

- **Per-function fixed overhead**: every function reads `WASMEDGE_IR_JIT_OPT_LEVEL`
  from `getenv()` twice (once in `ir_builder.cpp:153`, once in
  `ir_jit_engine.cpp:134`), runs `ir_check()`, allocates/frees the IR context,
  and registers with GDB via `ir_gdb_register()`.  635 functions x ~50 us
  overhead = ~30 ms.

**2. IR-to-native compile phase (48% of time)**

- **Super-linear optimiser cost**: func 185 has only 4,857 instrs but takes
  19,780 us to compile -- 4x the per-instruction cost of func 34.  This is
  consistent with SCCP or register allocation hitting complex IR patterns
  (deeply nested PHI nodes, many merge points).

- **SCCP constant-pool pre-allocation**: the IR library has a latent bug where
  SCCP holds raw pointers during `ir_const()`, which can trigger realloc.  The
  current workaround (pre-allocating 256 constant slots in `ir_init()`)
  prevents crashes but doesn't eliminate the SCCP cost itself.

### Possible Instantiation Improvements

| Improvement | Est. savings | Effort |
|-------------|-------------|--------|
| Eliminate InstrVec copy (use Span directly) | 10-20 ms | Low |
| Cache `getenv()` calls (read once, not per-function) | 5-10 ms | Trivial |
| Lazy env-field loading (only load used fields) | 5-10 ms (build phase) | Low |
| Skip `ir_gdb_register` unless debug env set | 5 ms | Trivial |
| Parallel compilation (thread pool) | 200+ ms | High |
| Tiered: compile hot functions first, defer cold | Perceived 200+ ms | High |

---

## Part B: WorkTime Gap (1,022 ms vs 713 ms AOT -- 43% slower)

This is the more fundamental issue.  The generated code quality trails LLVM
AOT by 43%.  Three architectural root causes dominate.

### Root Cause 1: No Cross-Function Inlining (~20-25% of the gap)

Every wasm `call` instruction compiles to:

```
; Caller side (per call):
  STORE arg0 -> SharedCallArgs[0]     ; marshal each arg to memory
  STORE arg1 -> SharedCallArgs[1]
  ...
  LOAD_A FuncTable[calleeIdx]         ; load function pointer (indirect)
  CALL   funcptr(env, SharedCallArgs) ; indirect call

; Callee prologue (every function):
  LOAD args[0] -> local0              ; unmarshal args from memory
  LOAD args[1] -> local1
  ...
  LOAD env->FuncTable                 ; reload 8 env struct fields
  LOAD env->FuncTableSize
  LOAD env->GlobalBase
  LOAD env->MemoryBase
  LOAD env->HostCallFn
  LOAD env->MemoryGrowFn
  LOAD env->MemorySizeFn
  LOAD env->CallIndirectFn
```

There are **2,682 `CALL` instructions** across the module.  For a compression
workload with many small helper functions (hash lookups, bit manipulation,
buffer management), the overhead per call is:

- N stores + N loads for arg marshalling (vs register passing in AOT)
- Indirect call penalty (~5 cycles branch mispredict vs direct call)
- Callee prologue reloads 8 env fields that are loop-invariant

LLVM AOT **inlines** small functions entirely, eliminating all of this overhead.
The IR JIT's `IR_OPT_INLINE` flag only handles inlining within a single IR
graph (e.g. constant folding); it does not inline across wasm function
boundaries.

**Evidence:** Looking at the IR dump for a small function
(`/tmp/wasmedge_ir_050_after.ir`, ~100 lines), the function makes 3 indirect
calls to other wasm functions.  Each call involves 2-3 stores for arg
marshalling plus the indirect call sequence.  LLVM would inline these callees.

### Root Cause 2: ZEXT + ADD on Every Memory Access (~5-8% of the gap)

Every wasm linear-memory load/store generates:

```
ZEXT  wasmAddr_i32 -> wasmAddr_i64    ; zero-extend 32-bit address
ADD   memoryBase + wasmAddr_i64       ; compute effective address
LOAD  [effectiveAddr]                 ; actual memory access
```

With **15,774 `ZEXT` nodes** module-wide, this adds two extra instructions per
memory access.  LLVM can:

- Fold `ZEXT + ADD` into x86 addressing modes (e.g. `mov (%rax,%ebx), %ecx`
  with implicit zero-extension on 32-bit register operands)
- Prove upper bits are zero and eliminate ZEXT entirely
- Hoist invariant base-pointer additions out of loops

### Root Cause 3: Env Struct Field Reloading (~3-5% of the gap)

Every compiled function's prologue loads all 8 `JitExecEnv` fields
(`ir_builder.cpp:223-230`) regardless of whether the function uses them.  A
pure arithmetic function that never touches memory still loads `MemoryBase`,
`MemoryGrowFn`, `CallIndirectFn`, etc.

In a deep call stack (common in compression algorithms), these invariant values
are reloaded at every frame.  LLVM keeps them in callee-saved registers across
call boundaries.

### Minor Contributing Factors

- **No loop unrolling**: the IR backend does not unroll loops.  Compression
  inner loops (byte-by-byte scanning, hash chain walking) benefit significantly
  from unrolling.

- **No auto-vectorization**: LLVM can vectorize `memcpy`/`memset`-like patterns
  common in compression (buffer copies, CRC computation).  Not available in the
  IR backend.

- **No alias analysis**: the IR backend cannot prove that two memory accesses
  don't alias, preventing load/store reordering and elimination.

### WorkTime Improvement Priorities

| Factor | Est. impact on gap | Difficulty | Notes |
|--------|-------------------|------------|-------|
| Cross-function inlining | 20-25% | Hard | Requires multi-function IR compilation or function cloning |
| Register-based calling convention | 10-15% | Medium | Pass first N args in registers instead of memory buffer |
| ZEXT/addressing mode fusion | 5-8% | Medium | Teach IR backend to fold ZEXT+ADD into addressing modes |
| Lazy env-field loading | 3-5% | Easy | Only emit loads for env fields actually referenced |
| Loop unrolling | 2-5% | Medium | Add unroll pass to IR pipeline |

---

## Summary

The 43% WorkTime gap is dominated by the **function call overhead** (no
inlining + memory-based arg passing + env reloading).  This is an architectural
property of the current IR JIT design where each wasm function is compiled
independently with a uniform `func(env*, args*)` calling convention.

The 493 ms instantiation latency is split evenly between Wasm-to-IR translation
(which scales linearly with code size but has a high constant factor from IR
graph inflation) and the IR optimiser passes (which exhibit super-linear cost
on functions with complex control flow).

For rust-compression specifically, the workload is call-heavy (gzip + brotli
with many small utility functions), which maximises exposure to the call
overhead.  Compute-bound kernels with fewer cross-function calls would show a
smaller gap.
