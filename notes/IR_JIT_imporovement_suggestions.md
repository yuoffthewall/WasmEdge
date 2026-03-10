
Here are focused improvement suggestions for the IR JIT flow and code.

---

# IR JIT improvement suggestions

## 1. Performance

### 1.1 Invoke path: avoid rebuilding env and buffers every call

**Where:** `lib/executor/helper.cpp` (enterFunction IR JIT branch) and `lib/vm/ir_jit_engine.cpp` (invoke).

**Issue:** On every IR JIT call we:

- Build `FuncTableStorage`, `GlobalPtrStorage` from `ModInst` (loop over all functions/globals).
- Build a stack-allocated `JitExecEnv` and a `std::vector<uint64_t> ArgsRaw` in `invoke()`.

**Suggestion:**

- **Cache per-module JIT env:** For a given `ModuleInstance`, FuncTable and GlobalBase are fixed until the module is modified. Cache (e.g. on the module or in a thread-local map keyed by module) the built `void** FuncTable` and `void* GlobalBase` (and optionally `MemoryBase`) and reuse them for all invokes of that module’s JIT functions. Invalidate on table/element/global changes if/when you support that.
- **Reuse args buffer:** Use a thread-local (or executor-owned) `std::vector<uint64_t>` for `ArgsRaw`, `resize(ParamTypes.size())` per invoke instead of allocating a new vector. Same for any similar temporary in the hot path.

This cuts per-call allocation and repeated iteration over all functions/globals.

### 1.2 Single IR JIT engine and TLS

**Where:** `lib/executor/helper.cpp` line 432, `lib/executor/instantiate/module.cpp` line 145.

**Issue:** `static VM::IRJitEngine IREngine` is defined in two translation units. You effectively have two separate engines (instantiation vs execution). TLS (`g_jitExecutor`, etc.) is separate from engine lifetime.

**Suggestion:**

- Use **one** IR JIT engine instance: e.g. owned by the `Executor` or by a dedicated “JIT runtime” used by both instantiation and execution, and pass it (or get it from a known place) in both `instantiate` and `enterFunction`. That avoids duplicate statics and makes it clear where the engine lives.
- Keep TLS only for what truly must be thread-local for `jit_host_call` (executor, stack, module, table). Document that the engine (or its code buffers) is shared and safe for the chosen threading model.

### 1.3 valVariantToRaw must be type-correct (F32/F64)

**Where:** `lib/vm/ir_jit_engine.cpp` lines 193–197.

**Issue:** `valVariantToRaw` uses `Val.get<uint64_t>()` for all types. For F32/F64 the bits must be preserved (e.g. via memcpy), not reinterpreted as integer.

**Suggestion:** Mirror `rawToValVariant` with a type-aware conversion. `invoke` already has `ParamTypes`; use it:

- e.g. `valVariantToRaw(Val, ParamTypes[i])` and, per type: I32/I64 → integer get; F32/F64 → memcpy into a `uint64_t`. That keeps JIT ABI correct and avoids subtle bugs when floats are passed.

---

## 2. Correctness and efficiency (concise implementation)

### 2.1 Bounds checking

**Where:** `lib/vm/ir_builder.cpp` lines 1428–1434, `buildBoundsCheck`.

**Issue:** Bounds check is a no-op; out-of-bounds memory can corrupt or crash.

**Suggestion:** Add a real bounds check when you’re ready (e.g. compare `Address + AccessSize` to `MemorySize`). Pass `MemorySize` (or a safe upper bound) into the builder/JitExecEnv so the generated code can do a single comparison or call a small helper, without sacrificing too much performance. Optionally make it configurable (e.g. debug vs release).

### 2.2 Div/rem_u helpers

**Where:** `lib/vm/ir_builder.cpp` (e.g. wasm_i32_div_u, wasm_i64_rem_u).

**Issue:** Repeated `ir_proto_2` + `ir_const_func_addr` + `ir_CALL_2` for the same helper in multiple opcodes.

**Suggestion:** Build **once per function** (or once per context) the proto and function address for each helper (e.g. `wasm_i32_div_u`, `wasm_i64_div_u`, …), store refs in the builder, and reuse them in visitBinary/visitCompare. Shrinks code and avoids repeated IR for the same C helper.

---

## 3. Code quality and structure

### 3.1 Instantiation: extract “module context” and pre-pass

**Where:** `lib/executor/instantiate/module.cpp` (the big `#ifdef WASMEDGE_BUILD_IR_JIT` block).

**Issue:** One long block that: collects FuncTypes, TypeSection, GlobalTypes, ImportFuncNum, CodeSegs, SkipJit pre-pass, then the per-function loop with init/build/compile/upgrade. Hard to read and test.

**Suggestion:**

- **Struct for “IR JIT module context”:** e.g. `IRJitModuleContext { FuncTypes, TypeSection, GlobalTypes, ImportFuncNum, CodeSegs, SkipJit, TotalDefined }`. Fill it once from `Mod` and `ModInst`.
- **Pre-pass function:** e.g. `computeSkipJit(CodeSegs, ImportFuncNum)` returning `std::vector<bool> SkipJit`. Keeps the “why we skip” logic in one place.
- **Per-function compile:** a function like `tryCompileFunction(IRBuilder, IREngine, Mod, ModInst, FuncIdx, CodeSeg, …)` that does init, setModule*, pre-scan MaxCallArgs, build, compile, upgrade; return success/fail. The main loop becomes: for each code segment, if !SkipJit[FuncIdx], tryCompileFunction(…); else skip.

This keeps instantiation readable and allows unit tests for skip logic and single-function compile.

### 3.2 IR builder: visitor structure and opcode tables

**Where:** `lib/vm/ir_builder.cpp` (visitBinary, visitCompare, visitUnary, visitConversion, visitMemory).

**Issue:** Very long switch statements; a lot of repetitive “pop operands, one IR op, push”. Adding a new opcode touches a large switch.

**Suggestion:**

- **Opcode → IR mapping tables:** For simple binary ops (e.g. I32 add/sub/mul, I64 add/sub/mul, F32/F64 add/sub/mul), use a table or macro that maps `OpCode` to “left type, right type, result type, IR builder lambda or function.” Then a single loop or small switch can handle many opcodes. Keep special cases (div_u, rem_u, calls to helpers) as explicit branches.
- **Shared “binary op” helper:** e.g. `visitBinarySimple(Left, Right, Op)` that handles the common “two operands, one result” pattern and delegates to the table or to a small set of helpers (int vs float, signed vs unsigned). Reduces duplication and keeps visitBinary shorter.

Same idea can be applied to compare/unary/conversion where many opcodes follow the same pattern.

### 3.3 Centralize JIT env and call-convention details

**Where:** `JitExecEnv` and its layout, `helper.cpp` (FuncTable/GlobalBase build), `ir_builder.cpp` (EnvPtr, FuncTablePtr, …).

**Issue:** Offsets and layout are implied by `offsetof(JitExecEnv, …)` and by how the builder and executor build the env. Adding a field or changing layout requires touching several files.

**Suggestion:**

- **Single place for env layout:** e.g. in `ir_jit_engine.h` (or a small `ir_jit_abi.h`), document `JitExecEnv` layout and the uniform calling convention (ret func(JitExecEnv*, uint64_t*)). Use static_assert on offsetof if needed.
- **Builder env setup in one function:** In the builder, one function (e.g. `loadEnvFromParams(EnvPtr)`) that sets FuncTablePtr, FuncTableSize, GlobalBasePtr, MemoryBase, HostCallFnPtr from EnvPtr. Called from initialize(). Makes it obvious what the generated code expects from the env.

### 3.4 Remove redundant comments and update misleading ones

**Where:** `lib/executor/instantiate/module.cpp` lines 175–181, 233–234.

**Issue:** Comment still describes “transitive skip” and “non-JIT call target” as the reason for SkipJit, but transitive skip was removed; only “trap stub” (and imports in the index range) are set now.

**Suggestion:** Short comment before the pre-pass: “Skip only: imports (indices < ImportFuncNum) and trap stubs (body starts with unreachable).” Update the “skip func” log message to e.g. “skip func %u (trap stub or import)” so it matches actual behavior and stays useful for debugging.

---

## 4. Scalability and maintainability

### 4.1 Split ir_builder.cpp by category

**Where:** `lib/vm/ir_builder.cpp` (~2400 lines).

**Issue:** One large file with instruction visitors, control flow, memory, calls, and helpers. Hard to navigate.

**Suggestion:** Split by concern (without changing public API):

- **ir_builder_core.cpp:** initialize, reset, stack/locals, getOrEmitReturnValue, ensureValidRef, coerceToType, wasmTypeToIRType, mergeLocals, mergeResults, emitLoopBackEdge.
- **ir_builder_control.cpp:** visitBlock, visitLoop, visitIf, visitElse, visitEnd, visitBr, visitBrIf, visitBrTable, visitReturn, visitControl.
- **ir_builder_arith.cpp:** visitConst, visitLocal, visitGlobal, visitBinary, visitCompare, visitUnary, visitConversion, visitParametric.
- **ir_builder_memory.cpp:** buildMemoryAddress, buildBoundsCheck, visitMemory.
- **ir_builder_call.cpp:** visitCall.

Keep `visitInstruction` and the main builder in one file that includes or links the rest. This improves readability and makes it easier to add new opcodes or change control-flow handling.

### 4.2 Optional tier-up / profiling

**Where:** `IRJitEngine::CompileResult` already keeps `IRGraph`; `release` doesn’t use it.

**Suggestion:** If you plan tier-up (e.g. recompile hot functions with more optimization):

- Keep the IR graph (or a serialized form) when you want tier-up, and release it when the module is unloaded or when you decide not to tier up.
- Add a minimal hook (e.g. “on function return” or “periodic”) to count invocations or sample; use that to decide which functions to recompile. This can be behind a compile-time or runtime flag so it doesn’t affect the default path.

---

## 5. Summary table

| Area | Suggestion | Benefit |
|------|------------|--------|
| **Performance** | Cache FuncTable/GlobalBase per module; reuse args buffer | Fewer allocations and less work per invoke |
| **Performance** | Single IR JIT engine instance; clear ownership | No duplicate statics; predictable lifetime |
| **Correctness** | Type-correct valVariantToRaw (F32/F64) | Correct JIT ABI for floats |
| **Correctness** | Real bounds check (configurable) | Safety without large refactors later |
| **Efficiency** | Reuse div/rem_u proto and func addr in builder | Less IR and cleaner code |
| **Structure** | Module context + pre-pass + tryCompileFunction in instantiate | Readable, testable instantiation |
| **Structure** | Opcode tables + small helpers in ir_builder | Shorter switches; easier to add opcodes |
| **Structure** | Document JitExecEnv and env setup in one place | Clear ABI and fewer bugs when changing layout |
| **Comments** | Update skip-logic comments and log message | Matches behavior; easier debugging |
| **Scalability** | Split ir_builder.cpp by category | Easier navigation and changes |
| **Future** | Optional tier-up using preserved IRGraph | Path to better performance for hot code |

If you want to prioritize, the highest impact with limited change are: **type-correct valVariantToRaw**, **caching/reuse of FuncTable/GlobalBase and args buffer**, and **single IR JIT engine instance**. Then add **module context + pre-pass extraction** and **ir_builder split** for maintainability.