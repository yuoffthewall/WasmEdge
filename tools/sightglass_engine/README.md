# sightglass_engine

A sightglass-cli engine plugin that runs the WasmEdge IR JIT (and the
LLVM JIT / Interpreter, for comparison) under sightglass's measurement
harness.

The library exports the five C symbols sightglass's recorder
(`crates/recorder/src/bench_api.rs`) `dlopen`s:

```
wasm_bench_create   wasm_bench_free
wasm_bench_compile  wasm_bench_instantiate  wasm_bench_execute
```

Sightglass times the compilation, instantiation, and execution phases
separately. The shim wires `bench:start` / `bench:end` (the host module
upstream sightglass benchmarks call to mark the measured region) to the
sightglass-provided execution-phase timer callbacks, so execution timing
narrows to the bench-marked region inside the wasm rather than the
whole `_start` body.

## Build

The shim is built whenever `WASMEDGE_BUILD_IR_JIT=ON`:

```sh
cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DWASMEDGE_BUILD_IR_JIT=ON \
      ..
make wasmedge_engine -j32
# → build/tools/sightglass_engine/libwasmedge_engine.so
```

## Use

```sh
SG=$HOME/Desktop/sightglass/target/release/sightglass-cli
ENGINE=$(pwd)/build/tools/sightglass_engine/libwasmedge_engine.so

WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
"$SG" benchmark \
  --engine "$ENGINE" \
  --processes 3 \
  --output-format json \
  ../path/to/benchmark.wasm
```

Mode and other engine knobs are read from environment variables (the
same set the in-tree harness uses):

| Variable | Effect |
|---|---|
| `WASMEDGE_SIGHTGLASS_MODE` | `Interpreter` / `IR_JIT` (default) / `JIT` |
| `WASMEDGE_IR_JIT_OPT_LEVEL` | `0` / `1` / `2` (default) / `3` |
| `WASMEDGE_TIER2_ENABLE` | `1` to enable tier-2 background compilation |
| `WASMEDGE_TIER2_THRESHOLD` | function-entry hotness count for tier-2 |
| `WASMEDGE_OSR_THRESHOLD` | back-edge iterations for OSR (`0` disables) |

Sightglass forks a new process per measurement, so all env vars are
captured per invocation.

## ABI version

`wasm_bench_config.h` mirrors `WasmBenchConfig` from sightglass's
`crates/recorder/src/bench_api.rs`. The `static_assert` in that header
catches struct-size drift; bump the pinned sightglass commit and
re-validate when upstream changes the layout.

## Modes

All four engine modes work end-to-end through the dlopen'd shim:

- `WASMEDGE_SIGHTGLASS_MODE=Interpreter`
- `WASMEDGE_SIGHTGLASS_MODE=IR_JIT` (tier-1)
- `WASMEDGE_SIGHTGLASS_MODE=IR_JIT` + `WASMEDGE_TIER2_ENABLE=1` (tier-2)
- `WASMEDGE_SIGHTGLASS_MODE=IR_JIT` + `WASMEDGE_TIER2_ENABLE=1` +
  `WASMEDGE_OSR_THRESHOLD=5000` (tier-2 + OSR)
- `WASMEDGE_SIGHTGLASS_MODE=JIT` (whole-module LLVM JIT)

A previous version of this README documented a TLS-model bug that
caused tier-2 promotion to segfault when WasmEdge was loaded as a
shared library. That has been fixed: tier-2 codegen now calls the
ORC-bound `wasmedge_tier2_get_jit_env` / `wasmedge_tier2_get_exec_ctx`
accessors instead of emitting `%fs:OFFSET` inline asm, so TLS access
goes through the compiler's correct sequence regardless of how
WasmEdge is linked into the host process.
