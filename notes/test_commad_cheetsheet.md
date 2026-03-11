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
  stdbuf -oL timeout 15 ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*' 2>&1
done | tee /tmp/wasm-test.log
```