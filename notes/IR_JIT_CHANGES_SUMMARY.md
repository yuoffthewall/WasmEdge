# IR JIT changes summary

## 1. Stack overflow fix (shootout-ed25519)

**Problem:** Every `call` instruction used `ir_ALLOCA` to allocate the callee args buffer on the native stack but never freed it. Functions with many calls (e.g. `__original_main` with 887 calls) accumulated unbounded stack usage (~1.4MB per iteration), causing segfault.

**Solution:** Pre-allocate a single reusable args buffer per function.
- **include/vm/ir_builder.h:** Added `MaxCallArgs`, `SharedCallArgs`, and `setMaxCallArgs()`.
- **lib/vm/ir_builder.cpp:** In `buildFromInstructions()`, emit one `ir_ALLOCA(MaxCallArgs * 8)` at function entry. In `visitCall()`, use `SharedCallArgs` instead of per-call `ir_ALLOCA`.
- **lib/executor/instantiate/module.cpp:** Pre-scan function instructions to compute max parameter count across all calls; call `IRBuilder.setMaxCallArgs(MaxArgs)` before building.

## 2. WASI host call context (shootout-matrix)

**Problem:** When JIT code called WASI host functions (e.g. `fd_fdstat_get`) via `jit_host_call`, the trampoline used `runFunction()`, which pushes a dummy frame with `nullptr` module. Host functions then got the wrong `CallingFrame` (WASI module instead of caller’s module) and could not access the benchmark module’s memory → HostFuncError 0x40e.

**Solution:** Add a dedicated `jitCallFunction` that passes the caller’s module in the dummy frame.
- **include/executor/executor.h:** New `jitCallFunction(StackMgr, Func, Params, CallerMod)` taking `CallerMod`.
- **lib/executor/helper.cpp:** Implement `jitCallFunction` by pushing `CallerMod` (instead of `nullptr`) in the dummy frame, then pushing params and calling `enterFunction` / `execute`. `jit_host_call` calls `jitCallFunction(..., g_jitModInst)`.

## 3. call_indirect fast path (stack depth)

**Problem:** Every `call_indirect` went through `jit_host_call` → `jitCallFunction` → full executor path, adding deep C++ stack frames and contributing to stack pressure.

**Solution:** In `jit_host_call`, when the resolved target is an IR JIT function, call its native pointer directly (same signature as JIT: `(JitExecEnv*, uint64_t*)`), and only use the executor path for host/non-JIT targets.

- **lib/executor/helper.cpp:** In the `call_indirect` branch, after resolving `funcInst` from the table, if `funcInst->isIRJitFunction()` and `getIRJitNativeFunc()` is non-null, call that function with `(env, args)` and return; otherwise fall through to existing marshalling and `jitCallFunction`.

## 4. JitExecEnv and host-call trampoline (existing design, referenced)

- **include/vm/ir_jit_engine.h:** `JitExecEnv` includes `HostCallFn` (pointer to `jit_host_call`). Declares `extern "C" uint64_t jit_host_call(JitExecEnv*, uint32_t funcIdx, uint64_t* args)`.
- **lib/executor/helper.cpp:** Defines `jit_host_call`, TLS (`g_jitExecutor`, `g_jitStackMgr`, `g_jitModInst`, `g_jitTable0`), and uses them for direct vs indirect and JIT vs host dispatch.
- **lib/vm/ir_builder.cpp:** Loads `HostCallFnPtr` from `JitExecEnv`; for host calls and `call_indirect` emits a call through that pointer with encoded `funcIdx` and `CalleeArgs`.

## 5. Safe table access (modules without tables, e.g. noop)

**Problem:** In the JIT path we set `g_jitTable0 = getTabInstByIdx(StackMgr, 0)`, which uses `ModInst->unsafeGetTable(0)`. Modules with no table (e.g. noop) have empty `TabInsts`; indexing 0 is undefined behavior.

**Solution:** Use the safe API and only set a table when present.
- **lib/executor/helper.cpp:** Replace `g_jitTable0 = getTabInstByIdx(StackMgr, 0)` with: if `ModInst`, then `auto tabRes = ModInst->getTable(0)` and `g_jitTable0 = tabRes ? *tabRes : nullptr`; else `g_jitTable0 = nullptr`.

## 6. Module type section and import count (call_indirect / host calls)

- **include/vm/ir_builder.h:** `ModuleTypeSection` (types by type index), `ImportFuncNum`, `setModuleTypes()`, `setImportFuncNum()`, and `HostCallFnPtr` (loaded from env).
- **lib/executor/instantiate/module.cpp:** Pass type section and import count into IR builder: `IRBuilder.setModuleTypes(TypeSection)`, `IRBuilder.setImportFuncNum(ImportFuncNum)`.
- **lib/vm/ir_builder.cpp:** Use `ModuleTypeSection` for `call_indirect` type resolution; use `ImportFuncNum` to decide host vs direct JIT call; load and use `HostCallFnPtr` for host calls.

## 7. Transitive skip removal

- **lib/executor/instantiate/module.cpp:** Removed the logic that skipped JIT compilation for functions that (transitively) call imports or use `call_indirect`. All such calls now go through `jit_host_call` (or the call_indirect fast path), so those functions can be JIT-compiled.

## 8. Multiple loop back-edges (shootout-matrix control flow)

- **include/vm/ir_builder.h:** `LabelInfo` extended with `LoopBackEdgeEnds` and `LoopBackEdgeLocals` to support multiple back-edges per loop.
- **lib/vm/ir_builder.cpp:** `emitLoopBackEdge` and `visitEnd` updated to collect and merge multiple loop back-edges and PHI nodes so CFG and PHIs are correct (fixes `bb->successors_count == 1` assertion in ir_x86.dasc for dlfree-style loops).

## Resulting behavior

- **shootout-ed25519:** Passes (no stack overflow; shared args buffer + call_indirect fast path).
- **shootout-matrix:** Passes (WASI gets correct CallingFrame; loop back-edge fix; shared args).
- **noop*:** Still crashes inside JIT-generated code (suspected calling convention or codegen); table fix avoids UB in C++ but does not fix the runtime crash.

## Files touched (for commit)

- include/executor/executor.h
- include/vm/ir_builder.h
- include/vm/ir_jit_engine.h
- lib/executor/helper.cpp
- lib/executor/instantiate/module.cpp
- lib/vm/ir_builder.cpp
- lib/vm/ir_jit_engine.cpp

(Exclude from this commit: testdata deletions/renames, thirdparty/ir submodule, .cursor, notes, workspace file, unless intentionally part of the same change.)
