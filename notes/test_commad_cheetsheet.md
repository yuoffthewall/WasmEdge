# Build (use Debug for GDB visibility; -j32 for parallel)

```shell
cd /home/tommy/Desktop/wasmedge/build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DWASMEDGE_BUILD_IR_JIT=ON \
      -DWASMEDGE_BUILD_TESTS=ON \
      ..

cmake -DCMAKE_BUILD_TYPE=Debug .   # optional: better backtraces and JIT symbols

cmake --build . --target wasmedgeIRBenchmarkTests -j32
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