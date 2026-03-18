# Build Wasmedge (use Debug for GDB visibility; -j32 for parallel)

```shell
cd /home/tommy/Desktop/wasmedge/build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DWASMEDGE_BUILD_IR_JIT=ON \
      -DWASMEDGE_BUILD_TESTS=ON \
      ..

cmake -DCMAKE_BUILD_TYPE=Debug ..   # optional: better backtraces and JIT symbols

cmake --build . --target wasmedgeIRBenchmarkTests -j32
```

# Build thirdparty/ir
```shell
cd thirdparty/ir
CFLAGS=-fPIC BUILD_CFLAGS=-fPIC make BUILD=debug libir.a    # debug, PIC (shared lib friendly)
# or
CFLAGS=-fPIC BUILD_CFLAGS=-fPIC make BUILD=release libir.a
```

# Test a single sightglass .wasm kernel using IR JIT
Use quicksort as an example:

```shell
cd ~/Desktop/wasmedge/build && WASMEDGE_SIGHTGLASS_MODE=IR_JIT WASMEDGE_SIGHTGLASS_KERNEL=quicksort timeout 30 ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

# Test all sightglass .wasm kernel using IR JIT

```shell
cd /home/tommy/Desktop/wasmedge/build && \
for wasm in ../test/ir/testdata/sightglass/*.wasm; do
  kernel="$(basename "$wasm" .wasm)"
  echo "Testing $kernel:"
  WASMEDGE_SIGHTGLASS_SKIP_INTERP=1 \
  WASMEDGE_SIGHTGLASS_KERNEL="$kernel" \
  WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
  stdbuf -oL timeout 30 ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*' 2>&1
done | tee /tmp/wasm-test.log
```

# Debugging with GDB

Run a single kernel (e.g. quicksort) under GDB from the build dir. Use a Debug build for symbols and JIT unwind.

```shell
cd ~/Desktop/wasmedge/build
WASMEDGE_SIGHTGLASS_SKIP_INTERP=1 WASMEDGE_SIGHTGLASS_KERNEL=quicksort WASMEDGE_IR_JIT_OPT_LEVEL=0 gdb --args ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

In GDB: `run`. When you get SIGSEGV in JIT code (e.g. `wasm_jit_002`), the backtrace may stop at one frame; use the following to get more info:

| What you want | Command / approach |
|---------------|--------------------|
| Instruction that faulted | `x/i $pc` |
| Context around it | `disas $pc-32,$pc+32` |
| Bad pointer / register state | `info registers` and match the register used in the faulting instruction |
| Which Wasm function | `wasm_jit_NNN` = (NNN+1)th JIT’d function (000, 001, 002…). Confirm by breaking before the JIT call and re-running: `break WasmEdge::Executor::enterFunction` or `break WasmEdge::VM::IRJitEngine::invoke`, then `run` and inspect which function/kernel is being invoked when you continue to the crash. |


# IR dumps in **WasmEdge’s IR JIT path**

### Primary knob: `WASMEDGE_IR_JIT_DUMP`

If this env var is **set** (any non-empty value), `IRJitEngine::compile` in `ir_jit_engine.cpp` writes two files **per JIT-compiled Wasm function**, in compile order:

1. **Before** `ir_jit_compile`:  
   `/tmp/wasmedge_ir_NNN_before.ir`  
   - `ir_save(Ctx, 0, f)` → plain IR text (no extra CFG flags).

2. **After** successful `ir_jit_compile`:  
   `/tmp/wasmedge_ir_NNN_after.ir`  
   - `ir_save(Ctx, IR_SAVE_CFG, f)` → same IR dump **plus CFG-related annotations** (see `IR_SAVE_CFG` in `ir.h`).

`NNN` is a running index (`000`, `001`, …) for each compiled function in that process.

**Usage:** run WasmEdge with IR JIT on, e.g.:

`WASMEDGE_IR_JIT_DUMP=1 wasmedge ...`

Then inspect `/tmp/wasmedge_ir_*.ir`.

### Related debugging

- **`WASMEDGE_IR_JIT_OPT_LEVEL=0|1|2`** — matches what you pass into `ir_jit_compile` (default **2**). Use **0** or **1** when chasing codegen / fusion bugs; dumps still reflect the graph **as seen by the compiler** at that opt level.

- **GDB:** the engine registers JIT regions with **`ir_gdb_register`** as `wasm_jit_NNN`, so you can break/disassemble generated code in GDB.

### If you need more than “before / after compile”

The standalone **`thirdparty/ir`** **`ir`** binary supports many **`--save-ir-after-*`** flags (SCCP, regalloc, dot, etc.), but that’s **not** wired into WasmEdge today—only the env-driven **`ir_save`** pair above. To get per-pass dumps from WasmEdge you’d have to add more calls (e.g. different `ir_save` flags or hooks) in the JIT pipeline yourself.