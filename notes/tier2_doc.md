# Tier-2 Hot Function Recompilation: Progress & Status

**Date:** 2026-04-03
**Branch:** `ir_jit`

---

## Overview

Tier-2 is a background LLVM recompilation system for WasmEdge's IR JIT. Hot
functions detected at runtime via call counting are recompiled from the
preserved `ir_ctx*` graph through LLVM for deeper optimization (loop unrolling,
vectorization, LICM, etc.), then live-swapped into the function table.

```
Wasm bytecode
  |
  v
ir_ctx* (tier-1, at instantiation via dstogov/ir)
  |
  +--> ir_jit_compile() --> native code (tier-1, fast)
  |
  +--> [hot function detected at runtime]
  |      |
  |      v
  |    ir_emit_llvm() --> LLVM IR text
  |      |
  |      v
  |    LLVMParseIR --> LLVMRunPasses(O2) --> ORC LLJIT --> native code (tier-2)
  |      |
  |      v
  |    FuncTable[idx] = tier2_ptr  (atomic pointer swap)
  |
  v
Execution continues (tier-1 or tier-2 transparently)
```

---

## Implementation Status

### Phase 1: Profiling Infrastructure -- COMPLETE

- Call counters in tier-1 function prologues (IF/MERGE + counter check)
- Loop detection during IR building (`OpCode::Loop` sets `HasLoop`)
- Configurable thresholds via env vars
- `jit_tier_up_notify()` callback from JIT code to C++ runtime

### Phase 2: LLVM AOT Tier-2 Compiler -- COMPLETE

- `Tier2Compiler` class: `ir_ctx* -> ir_emit_llvm -> LLVM ORC LLJIT`
- Symbol registry (`jit_symbol_registry.h`) for resolving JIT helpers
- `LLVMOrcAbsoluteSymbols` for registering helper addresses with LLJIT
- LLVM IR dump support (`WASMEDGE_TIER2_DUMP_IR=1`)

### Phase 3: Background Compilation & Code Replacement -- COMPLETE

- `Tier2Manager`: single background thread, condvar-based queue
- Deduplication via `Seen_` set (prevent redundant compilations)
- Atomic pointer swap: `FuncTable[idx] = tier2_ptr`
- Counter saturation on enqueue (`UINT32_MAX` prevents re-triggering)

### Phase 4: Polish & Testing -- IN PROGRESS

- Fixed 8 bugs in `ir_emit_llvm.c` (see `notes/tier2_bug.md`)
- 4 remaining open bugs (ir backend, not blocking core functionality)
- Sightglass validation: 37/37 kernels pass at O2 (no regressions)

---

## Files Changed

### New files

| File | Purpose |
|------|---------|
| `include/vm/tier2_compiler.h` | Tier-2 LLVM compiler interface |
| `lib/vm/tier2_compiler.cpp` | ir_ctx -> LLVM IR -> ORC LLJIT pipeline |
| `include/vm/tier2_manager.h` | Background compilation manager interface |
| `lib/vm/tier2_manager.cpp` | Worker thread, queue, code swap |
| `include/vm/jit_symbol_registry.h` | Shared name->address map for JIT helpers |

### Modified files

| File | Changes |
|------|---------|
| `thirdparty/ir/ir_emit_llvm.c` | 8 bug fixes for LLVM IR emission correctness |
| `lib/vm/ir_builder.cpp` | Symbol registry, IR_FUNC migration, fastcall flag, tier-up prologue |
| `lib/executor/helper.cpp` | `jit_tier_up_notify`, `getTier2Manager` singleton |
| `lib/executor/instantiate/module.cpp` | Counter allocation, threshold config, IR graph preservation |
| `include/executor/executor.h` | `IRJitEnvCache` with `CallCounters` and `TierState` |
| `include/vm/ir_jit_engine.h` | `JitExecEnv` with counter/notify fields, `CompileResult` with `IRGraph` |
| `include/runtime/instance/function.h` | `IRJitFunction` with `IRGraph` field, `upgradeToIRJit()` |
| `lib/vm/CMakeLists.txt` | Conditional tier-2 sources, LLVM include dirs |

---

## Key Design Decisions

### 1. Named functions (IR_FUNC) instead of raw addresses (IR_FUNC_ADDR)

`ir_emit_llvm()` requires named function references to emit `declare @name()`
statements. All 21+ call sites in `ir_builder.cpp` were migrated from
`ir_const_func_addr()` to `ir_const_func()` + `ir_str()`. An `ir_loader` is
attached to the ir_ctx so `ir_jit_compile()` can still resolve names to
addresses at native codegen time.

The `loaderHasSym` callback always returns `false` — this forces `ir_emit_llvm`
to emit `declare` statements for all external functions, which is required for
the LLVM parser to resolve them.

### 2. Post-compilation IR graph preservation

The `ir_ctx` is heap-copied after `ir_jit_compile()` via `detachIRContext()`.
This is only done when tier-2 is enabled (`Tier2Threshold > 0`), avoiding
overhead in tier-1-only mode. The copy is stored in
`IRJitFunction::IRGraph` and freed in the destructor.

### 3. Atomic pointer swap for code replacement

`FuncTable[idx] = tier2_ptr` is a single 64-bit store, naturally atomic on
x86-64 and aarch64. No lock, fence, or safepoint needed. In-flight tier-1
calls complete safely because the old native code remains alive (owned by
`FunctionInstance`).

### 4. Not skipping empty blocks in LLVM emitter

The original `ir_emit_llvm` used `ir_skip_empty_target_blocks()` to optimize
away trivial jumps. This created PHI/predecessor mismatches. Our fix emits all
blocks (including empty ones as `br label %successor`) and lets LLVM's own
optimization passes eliminate the dead code. This is correct-by-construction
and zero-cost after LLVM optimization.

### 5. Hex format for float constants and special values

LLVM IR requires float constants to round-trip exactly. The original `%e`
format lost precision. All float constants now use hex-double format
(`0x%016PRIX64`), and double `inf`/`NaN` use the same. This matches LLVM's
own constant emission.

---

## Configuration

| Env Var | Default | Description |
|---------|---------|-------------|
| `WASMEDGE_TIER2_ENABLE` | `0` | Enable tier-2 (`1` to enable) |
| `WASMEDGE_TIER2_THRESHOLD` | `10000` | Call count for non-loop functions |
| `WASMEDGE_TIER2_LOOP_THRESHOLD` | `1000` | Call count for loop-bearing functions |
| `WASMEDGE_TIER2_DUMP_IR` | unset | Write `/tmp/tier2_*.ll` for each tier-2 function |
| `WASMEDGE_TIER2_MAX_COMPILE` | unset | Limit total tier-2 compilations (debug/bisect) |
| `WASMEDGE_IR_JIT_OPT_LEVEL` | `2` | Tier-1 optimization level (0/1/2) |
| `WASMEDGE_IR_JIT_DUMP` | unset | Dump tier-1 IR before/after compile to `/tmp/wasmedge_ir_NNN_{before,after}.ir` |
| `WASMEDGE_IR_JIT_BOUND_CHECK` | `1` | Enable/disable memory bounds checking |

---

## Test Commands

All commands assume you are in the `build` directory and
`wasmedgeIRBenchmarkTests` is up to date (`make wasmedgeIRBenchmarkTests -j32`).
If `thirdparty/ir` was modified, rebuild `libir.a` first
(`cd thirdparty/ir && CFLAGS=-fPIC BUILD_CFLAGS=-fPIC make BUILD=debug libir.a`).

### Baseline: single kernel, tier-1 only

```shell
WASMEDGE_SIGHTGLASS_KERNEL=quicksort \
WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
WASMEDGE_SIGHTGLASS_QUICK=1 \
WASMEDGE_IR_JIT_BOUND_CHECK=0 \
timeout 30 ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

### Baseline: all kernels, tier-1 only

```shell
for wasm in ../test/ir/testdata/sightglass/*.wasm; do
  kernel="$(basename "$wasm" .wasm)"
  echo "Testing $kernel:"
  WASMEDGE_SIGHTGLASS_KERNEL="$kernel" \
  WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
  WASMEDGE_SIGHTGLASS_QUICK=1 \
  WASMEDGE_IR_JIT_OPT_LEVEL=2 \
  WASMEDGE_IR_JIT_BOUND_CHECK=0 \
  stdbuf -oL timeout 30 ./test/ir/wasmedgeIRBenchmarkTests \
    --gtest_filter='*SightglassSuite*' 2>&1
done | tee /tmp/wasm-test.log

grep -E 'Assert|failed|dumped' /tmp/wasm-test.log || echo "All passed"
```

### Tier-2: single kernel

Use low thresholds (10/5) to force tier-up quickly during the test.

```shell
WASMEDGE_SIGHTGLASS_KERNEL=rust-compression \
WASMEDGE_TIER2_ENABLE=1 \
WASMEDGE_TIER2_THRESHOLD=10 \
WASMEDGE_TIER2_LOOP_THRESHOLD=5 \
WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
WASMEDGE_SIGHTGLASS_QUICK=1 \
WASMEDGE_IR_JIT_BOUND_CHECK=0 \
timeout 60 ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

Tier-2 compilation and swap messages appear on stderr:
```
[info] tier2: compiled wasm_tier2_239 → 0x7ffced1fc000
[info] tier2: upgraded func 239 → tier-2 (0x7ffced1fc000)
```

### Tier-2: all kernels

```shell
for wasm in ../test/ir/testdata/sightglass/*.wasm; do
  kernel="$(basename "$wasm" .wasm)"
  echo "Testing $kernel:"
  WASMEDGE_SIGHTGLASS_KERNEL="$kernel" \
  WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
  WASMEDGE_SIGHTGLASS_QUICK=1 \
  WASMEDGE_IR_JIT_OPT_LEVEL=2 \
  WASMEDGE_IR_JIT_BOUND_CHECK=0 \
  WASMEDGE_TIER2_ENABLE=1 \
  WASMEDGE_TIER2_THRESHOLD=10 \
  WASMEDGE_TIER2_LOOP_THRESHOLD=5 \
  stdbuf -oL timeout 30 ./test/ir/wasmedgeIRBenchmarkTests \
    --gtest_filter='*SightglassSuite*' 2>&1
done | tee /tmp/wasm-tier2-test.log

grep -E 'Assert|failed|dumped' /tmp/wasm-tier2-test.log || echo "All passed"
```

### Tier-2: profiling overhead only (no recompilation)

Enables counter instrumentation but sets thresholds too high for any
function to tier-up. Useful for measuring the counter-check overhead
in isolation.

```shell
WASMEDGE_SIGHTGLASS_KERNEL=rust-compression \
WASMEDGE_TIER2_ENABLE=1 \
WASMEDGE_TIER2_THRESHOLD=999999999 \
WASMEDGE_TIER2_LOOP_THRESHOLD=999999999 \
WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
WASMEDGE_SIGHTGLASS_QUICK=1 \
WASMEDGE_IR_JIT_BOUND_CHECK=0 \
timeout 60 ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

### Tier-2: dump LLVM IR for inspection

Writes `/tmp/tier2_<funcname>.ll` for every tier-2 compiled function.
Combine with `WASMEDGE_IR_JIT_DUMP=1` to also get tier-1 IR dumps
(`/tmp/wasmedge_ir_NNN_{before,after}.ir`).

```shell
WASMEDGE_SIGHTGLASS_KERNEL=quicksort \
WASMEDGE_TIER2_ENABLE=1 \
WASMEDGE_TIER2_THRESHOLD=10 \
WASMEDGE_TIER2_LOOP_THRESHOLD=5 \
WASMEDGE_TIER2_DUMP_IR=1 \
WASMEDGE_IR_JIT_DUMP=1 \
WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
WASMEDGE_SIGHTGLASS_QUICK=1 \
WASMEDGE_IR_JIT_BOUND_CHECK=0 \
timeout 30 ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'

ls /tmp/tier2_wasm_tier2_*.ll     # tier-2 LLVM IR per function
ls /tmp/wasmedge_ir_*_before.ir   # tier-1 IR before optimization
ls /tmp/wasmedge_ir_*_after.ir    # tier-1 IR after optimization
```

### Tier-2: bisect a crashing function with MAX_COMPILE

If tier-2 causes a crash, use `WASMEDGE_TIER2_MAX_COMPILE` to limit the
number of tier-2 compilations and binary-search for the culprit.

```shell
# Coarse sweep: find the range
for n in 0 10 20 30 40 50 60 70 80; do
  result=$(WASMEDGE_SIGHTGLASS_KERNEL=rust-compression \
    WASMEDGE_TIER2_ENABLE=1 \
    WASMEDGE_TIER2_THRESHOLD=10 \
    WASMEDGE_TIER2_LOOP_THRESHOLD=5 \
    WASMEDGE_TIER2_MAX_COMPILE=$n \
    WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
    WASMEDGE_IR_JIT_OPT_LEVEL=2 \
    WASMEDGE_SIGHTGLASS_QUICK=1 \
    WASMEDGE_IR_JIT_BOUND_CHECK=0 \
    timeout 30 ./test/ir/wasmedgeIRBenchmarkTests \
      --gtest_filter='*SightglassSuite*' 2>&1 | tail -1)
  echo "MAX_COMPILE=$n: $result"
done

# Fine sweep: e.g. crash between 50 and 60
for n in $(seq 51 59); do
  result=$(WASMEDGE_SIGHTGLASS_KERNEL=rust-compression \
    WASMEDGE_TIER2_ENABLE=1 \
    WASMEDGE_TIER2_THRESHOLD=10 \
    WASMEDGE_TIER2_LOOP_THRESHOLD=5 \
    WASMEDGE_TIER2_MAX_COMPILE=$n \
    WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
    WASMEDGE_IR_JIT_OPT_LEVEL=2 \
    WASMEDGE_SIGHTGLASS_QUICK=1 \
    WASMEDGE_IR_JIT_BOUND_CHECK=0 \
    timeout 30 ./test/ir/wasmedgeIRBenchmarkTests \
      --gtest_filter='*SightglassSuite*' 2>&1 | tail -1)
  echo "MAX_COMPILE=$n: $result"
done

# Identify the Nth compiled function from the log:
WASMEDGE_TIER2_MAX_COMPILE=57 ... 2>&1 | grep 'tier2: upgraded' | tail -1
# → "tier2: upgraded func 239 → tier-2 (0x...)"
```

### Debugging a tier-2 crash with GDB

```shell
WASMEDGE_SIGHTGLASS_KERNEL=rust-compression \
WASMEDGE_TIER2_ENABLE=1 \
WASMEDGE_TIER2_THRESHOLD=10 \
WASMEDGE_TIER2_LOOP_THRESHOLD=5 \
WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
WASMEDGE_SIGHTGLASS_QUICK=1 \
WASMEDGE_IR_JIT_BOUND_CHECK=0 \
gdb --args ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

In GDB:
```gdb
handle SIGSEGV stop
run

# On crash:
x/i $pc                           # faulting instruction
info registers                    # register state
bt                                # backtrace (may be shallow for JIT code)
frame 1                           # caller frame
disas $pc-60,$pc+20               # context around crash site

# Examine FuncTable (r12 or rbp typically holds FuncTable base):
x/1gx $r12+0xf38                  # check a specific FuncTable entry
printf "FuncTable[%d]\n", 0xf38/8 # compute function index from offset

# Watchpoint on a FuncTable entry to catch who writes NULL:
break tier2_manager.cpp:95        # break at first tier-2 swap
run
x/1gx Req.FuncTable+487           # check entry value
watch *(void**)(Req.FuncTable+487*8)  # hardware watchpoint
continue
```

### Function index mapping

FuncTable is indexed by **total function index** (imports + wasm). The
JIT function names (`wasm_jit_NNN`) use a sequential compilation index
that skips non-compiled functions.

```
Total func index = NumImports + WasmFuncIndex
JIT name index   = sequential among compiled functions (skips gaps)

Example (12 imports, wasm func 475 skipped):
  wasm_jit_466 → FuncTable[478]   (466 + 12)
  FuncTable[487] = NULL            (wasm func 475, skipped)
  wasm_jit_475 → FuncTable[488]   (actually wasm func 476, shifted by gap)
```

To find the number of imports: `FuncTableSize - NumWasmFuncs` (both
printed in the `IR JIT: Compiled N/M functions` log line and in the
`IRJitEngine::invoke` GDB breakpoint).

---

## Test Results

### Sightglass baseline (no tier-2): 37/37 kernels PASS at O2

No regressions from tier-2 code changes (fastcall flag, symbol registry, etc.).

### Tier-2 enabled (threshold=10, loop_threshold=5):

| Metric | Count |
|--------|-------|
| Tier-2 compilations succeeded | 372+ |
| Tier-2 functions live-swapped | 372+ |
| LLVM IR parse errors | 0 |
| Kernels passing | 34/37 |
| Kernels crashing (ir backend) | 3 (blake3-scalar, blind-sig, shootout-nestedloop) |

The 3 ir backend crashes are assertion failures in `ir_print_const`,
`ir_get_str`, and `ir_match_insn` — all within `thirdparty/ir`, triggered
only during the tier-2 `ir_emit_llvm()` call on the background thread.
These don't affect tier-1 execution.

---

## Open Bugs

See `notes/tier2_bug.md` for full details. Summary:

| # | Description | Category |
|---|-------------|----------|
| 9 | `ir_print_const` assertion with IR_FUNC nodes | ir backend |
| 10 | `ir_get_str` strtab not preserved after compilation | ir backend |
| 11 | NIY instruction assertion in ir_emit_llvm | ir backend |
| 14 | O1 crash with IF/MERGE (pre-existing) | ir backend |

---

## Fixed Bugs (this session)

| # | Description | Location |
|---|-------------|----------|
| 1 | `zext i32 to ptr` → `inttoptr` | ir_emit_llvm.c |
| 2 | Empty blocks / PHI predecessor mismatch | ir_emit_llvm.c |
| 3 | Malformed `select` for non-bool conditions | ir_emit_llvm.c |
| 4 | `llvm.cttz/ctlz` missing `i1` type | ir_emit_llvm.c |
| 5 | `bitcast i1 to i8` invalid cast | ir_emit_llvm.c |
| 6 | Float constants / inf / NaN formatting | ir_emit_llvm.c |
| 7 | Missing `x86_fastcallcc` on function def | ir_builder.cpp |
| 8 | LLVM native target not initialized | tier2_compiler.cpp |
| 9 | Shift amounts not masked (wasm/LLVM UB mismatch) | ir_emit_llvm.c |
| 12 | rust-compression/regex tier-2 segfault (shift UB) | ir_emit_llvm.c |
| 13 | Static ALLOCA misclassified as dynamic | ir_x86.dasc |

---

## Next Steps

1. **Fix ir backend bugs** (Bugs 9-11) — contribute patches to dstogov/ir:
   - Add `IR_FUNC` handling to `ir_print_const()`
   - Preserve strtab across `ir_jit_compile()`
   - Implement missing instruction opcodes in `ir_emit_llvm()`

3. **Memory management** — the current LLJIT instances are intentionally leaked.
   Implement proper lifetime management in `Tier2Manager` (track LLJIT
   instances, dispose on module unload).

4. **Performance measurement** — compare tier-1 vs tier-2 execution time on
   long-running sightglass kernels to quantify the optimization benefit.

5. **Threshold tuning** — profile real workloads to find optimal call count
   thresholds. Current defaults (10000/1000) are reasonable starting points.
