# Commit summary: IR JIT fixes and call_indirect work

## 1. Review of changes

### Noop / proc_exit (fixed)
- **Terminated unwinding**: When a host function (e.g. `proc_exit`) returns `Terminated`, we no longer call `StackMgr.reset()` when the stack has ≥2 frames (JIT caller + our frames). We do two `popFrame()` so the JIT caller’s frame stays and we don’t segfault on return.
- **longjmp on Terminated**: In `jit_host_call`, when the result is `Terminated`, we `longjmp` to a buffer provided by `wasmedge_ir_jit_get_termination_buf()`. In `IRJitEngine::invoke()` we `setjmp` before calling the JIT; if longjmp is taken we return `Unexpect(Terminated)`. This avoids returning into JIT “unreachable” code after proc_exit.
- **No error log for Terminated**: We only call `spdlog::error` when the error is not `Terminated`, so proc_exit doesn’t print “[error]”.

### Direct call safety
- **jit_direct_or_host trampoline**: Direct `call` from JIT no longer calls the table entry directly. We go through `jit_direct_or_host(env, funcPtr, funcIdx, args, retTypeCode)`. If `funcPtr` is null (import or skipped function), we dispatch via `jit_host_call` instead of crashing.
- **JitExecEnv**: Added `DirectOrHostFn`; engine sets it in `invoke()`.
- **IR builder**: Direct-call path emits a call to `jit_direct_or_host` with 5 args and correct return handling (trunc/float/double).

### Engine and cache
- **Single IR JIT engine**: Replaced `static IRJitEngine` in instantiate with `getIRJitEngine()` on the Executor. Engine is stored in `Executor::IRJitEngine_` (lazy init).
- **IRJitEnvCache**: Per-module cache of FuncTable, GlobalPtrs, MemoryBase in Executor; `enterFunction` uses it instead of rebuilding every time.
- **ArgsBuffer_**: Reusable buffer in `IRJitEngine` for marshalling args in `invoke()`.
- **valVariantToRaw**: Now takes `ValType` and correctly handles F32/F64 (no longer truncates to integer).

### call_indirect / builder bookkeeping
- **CurrFuncNumParams**: Added in IR builder (set in `initialize()`, cleared in `reset()`) for possible future use (e.g. table-index-from-local heuristics). Not used for dispatch in this commit.

### Debug instrumentation (optional to remove later)
- **#region agent log** blocks in `helper.cpp`, `ir_jit_engine.cpp`, `instantiate/module.cpp` write NDJSON to `.cursor/debug-d32b78.log`. Safe to remove or guard behind a compile-time or env flag in a follow-up.

### Testdata
- **pulldown-cmark.wasm**: Deleted under `test/ir/testdata/sightglass/` (shown as deleted in diff).

### Not included in this commit (untracked / out of scope)
- `thirdparty/ir` submodule changes
- New notes (e.g. `current_bug.md`, `phi_node_bug_audit_38bff412.plan.md`)
- Other testdata (noop*.wasm, quicksort, etc.) and workspace files

---

## 2. Suggested commit message

```
[IR JIT] Fix proc_exit/Terminated, add jit_direct_or_host, single engine and cache

- Terminated: pop two frames (host + dummy) when nFrames>=2 instead of
  reset(), and longjmp from jit_host_call to invoke() so we don't return
  into JIT after proc_exit. Don't log error for Terminated.
- Add jit_direct_or_host(env, funcPtr, funcIdx, args, retTypeCode);
  direct JIT calls go through it; null funcPtr dispatches via jit_host_call.
- JitExecEnv: add DirectOrHostFn; IR builder uses it for direct calls with
  correct return-type handling.
- Single IR JIT engine per Executor (getIRJitEngine()), per-module
  IRJitEnvCache (FuncTable, GlobalBase), reusable ArgsBuffer_ in engine.
- valVariantToRaw(Val, Type) for correct F32/F64 in invoke().
- Add CurrFuncNumParams in IR builder for future call_indirect work.
- Debug logging to .cursor/debug-d32b78.log left in place (can remove later).

Noop-style Sightglass kernels pass with IR JIT. call_indirect kernels
(quicksort, heapsort, seqhash, ratelimit) still fail; table index
correctness is under investigation.
```
