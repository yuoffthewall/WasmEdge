# Why __original_main was interpreted before ffdbbeea, and why every call went through full executor dispatch

## Before commit ffdbbeea

### 1. Why __original_main was not JIT-compiled (so it was interpreted)

**Transitive skip (pre-ffdbbeea):** The instantiation logic did a fixed-point pass that marked a function as `SkipJit` if it:

- Uses **call_indirect** (or return_call_indirect), or  
- **Calls** (directly or transitively) any function that is already `SkipJit`.

Imports and trap stubs were `SkipJit`; then any function that calls an import or uses `call_indirect` was also marked `SkipJit`.

**So __original_main was skipped because:**

- It **calls imports**: e.g. `bench_start`, `bench_end` (and in matrix, `printf`, WASI, etc.). Those are host/import indices → `SkipJit`.
- It may **use call_indirect**: e.g. shootout-matrix’s `_black_box` via the table. Any `call_indirect` made the function `SkipJit` (table could hold host refs → nullptr in the JIT func table).

So in the pre-ffdbbeea fixed-point, __original_main was marked `SkipJit` and **never JIT-compiled** → at runtime it ran in the **interpreter** (the `else` branch in `enterFunction` that returns `getInstrs().begin()`).

(Additionally, in shootout-ed25519, __original_main had 887 call sites; before the shared call-args buffer, building it could hit stack overflow from many `ir_ALLOCA`s, so it could **fail to build** and also fall back to interpreter for that reason.)

---

### 2. Why every call from __original_main to a JIT function went through “full executor dispatch”

**Call path when the caller is the interpreter:**

1. **Interpreter** is running `execute(StackMgr, PC, End)` for __original_main (bytecode).
2. Current instruction is **call** (e.g. `call $fe25519_mul`).
3. Interpreter dispatch invokes **runCallOp** (in `controlInstr.cpp`):
   - `FuncInst = getFuncInstByIdx(StackMgr, Instr.getTargetIndex());`
   - `enterFunction(StackMgr, *FuncInst, PC + 1, IsTailCall);`
   - `PC = NextPC - 1;`
4. So **every** call from the interpreter goes through **enterFunction**.

There is no shortcut in the interpreter for “callee is JIT → call native pointer directly.” The interpreter only ever does: decode instruction → run the op’s handler → the handler for `call` is “call enterFunction.” So:

- **enterFunction** is the single entry point for invoking any function from the executor.
- For an IR JIT callee, enterFunction: builds FuncTable / GlobalBase / MemoryBase, sets TLS for `jit_host_call`, calls **IREngine.invoke(NativeFunc, …)**, pops frame, returns.

So each of the hundreds of calls per iteration (e.g. __original_main → fe25519_mul, __multi3, …) did:

- Interpreter decode **call**
- **runCallOp** → **enterFunction** → full setup (FuncTable, TLS, etc.) → **IREngine.invoke** → popFrame → return to interpreter.

That’s the “full executor dispatch” per call: we’re in C++, going through the full enterFunction path every time, instead of being inside JIT code and doing a direct call through the JIT function table (native → native with no enterFunction).

---

### 3. What changed in ffdbbeea

- **Transitive skip removed:** Functions that call imports or use call_indirect can now be JIT-compiled; calls go through **jit_host_call** (and call_indirect can use the JIT table / fast path). So __original_main is no longer forced to SkipJit and **can** be JIT-compiled.
- **Shared call-args buffer:** One pre-allocated args buffer per function (MaxCallArgs from a pre-scan) instead of ir_ALLOCA per call → avoids stack overflow in __original_main with 887 calls, so it can **build** and be JIT-compiled.

Once __original_main is JIT-compiled, a call from it to fe25519_mul (or any other JIT function) is a **direct call through the JIT function table** (native code → native code). No enterFunction, no C++ dispatch for those calls — only when we hit an import or a non-JIT target do we go through jit_host_call / executor.

---

## Summary

| Before ffdbbeea | Why |
|-----------------|-----|
| __original_main is **interpreted** | Transitive skip: it calls imports and/or uses call_indirect → marked SkipJit; optionally also build failed (ed25519 887 calls). |
| Every call from it to a JIT function goes through **full executor dispatch** | Caller is the interpreter; the only way to invoke the callee is runCallOp → enterFunction → full setup → IREngine.invoke. No “interpreter → direct native call” path. |

After ffdbbeea, __original_main can be JIT-compiled, so those hot calls become direct native→native calls via the JIT table instead of full enterFunction each time.
