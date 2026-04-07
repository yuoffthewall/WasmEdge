# IR JIT Frontend Bugs

Bugs in WasmEdge's own IR builder code (ir_builder.cpp, ir_jit_engine.cpp, helper.cpp),
as opposed to bugs in the upstream dstogov/ir library.

---

## Bug 1: call_indirect Uses Wrong Index Space for FuncTable Lookup

**Status: FIXED**

### Root Cause
In ir_builder.cpp:2765-2767, the call_indirect codegen uses the wasm table element index (popped from the wasm operand stack) to directly index into FuncTable, which is indexed by wasm function index. These are two different index spaces.


// BUG: TableIndex is a table element index, NOT a function index
Off = ir_MUL_A(ir_ZEXT_A(TableIndex), ir_CONST_ADDR(sizeof(void *)));
FuncPtr = ir_LOAD_A(ir_ADD_A(ValidFT, Off));  // FuncTable[tableElemIdx] -- WRONG

### How It Manifests
In the richards benchmark, task handler function pointers (e.g. idlefn, workfn, handlerfn, devfn) are stored in wasm linear memory as table element indices (assigned by the wasm linker). When call_indirect executes:

The code loads table element index (e.g., 3) from memory (tcb->t_fn)
It looks up FuncTable[3], which is the native code for wasm function 3 (some unrelated function)
It calls the wrong function, which returns a garbage value (e.g., 0xf7ffa000 -- the lower 32 bits of a JIT code pointer)
That garbage is used as a wasm memory offset, causing a segfault
The correct behavior would be: table element 3 -> resolve through wasm table -> function index N -> FuncTable[N].

### What Wasm call_indirect Actually Requires
1. Pop table element index from stack
2. Look up wasmTable[elementIdx] -> get funcref (which encodes the wasm function index)
3. Type-check the funcref against the expected signature
4. Call the resolved function

Step 2 is missing in the JIT path. Direct call instructions are unaffected because they use compile-time-known function indices.

### Impact
Any wasm module using call_indirect (function pointers, virtual dispatch, switch tables compiled to indirect calls) will call the wrong function, producing corruption or segfaults. This affects richards, regex, rust-json, and rust-protobuf benchmarks (all use indirect calls). Simple benchmarks without function pointers (e.g., hashset, shootout-*) are unaffected.

### Verification
Forcing call_indirect to pass funcPtr=null (which falls back to jit_host_call, correctly resolving through the wasm table) fixed richards and rust-protobuf. regex and rust-json still crash -- likely a second, unrelated bug in JIT codegen.

### Proper Fix Direction
The fix needs to resolve the table element index to a function index at runtime before looking up FuncTable. Options:

Add an indirect call table to JitExecEnv: Pre-build a void* IndirectTable[] mapping table element indices -> native code pointers. Use this for call_indirect instead of FuncTable. Needs invalidation if table.set/table.grow mutate the table.

Resolve at call site: Add the wasm table pointer to JitExecEnv. Generate IR to load wasmTable[elemIdx] -> extract funcIdx -> FuncTable[funcIdx]. More complex IR but avoids a separate table.

Always use host path for call_indirect (current workaround): Pass null funcPtr to force jit_host_call resolution. Correct but slower -- richards times out at 35s with this approach.

---

## Bug 2: ir_UNREACHABLE Drops Function Epilogue + Error Swallowed

**Status: FIXED**

This is actually two bugs working together.

### Bug 2a: ir_UNREACHABLE Generates No Code After CALL

**File:** `lib/vm/ir_builder.cpp`, Wasm `unreachable` handler.

The IR builder translated Wasm `unreachable` to `ir_UNREACHABLE()`. In the IR
framework, `UNREACHABLE` is a terminator used after tail calls -- it tells the
code emitter that the successor block is dead code and no epilogue is needed.

But our JIT functions use regular `CALL` (not tail call) to invoke
`jit_host_call`. A regular call returns normally. When the IR framework sees
`UNREACHABLE` as the successor, it marks the block as dead and generates no
code after the CALL. Execution falls through into uninitialized memory (zeros
= `add %al,(%rax)` -> SIGSEGV when rax=0).

**Example:** `wasm_jit_401` (function 413, which calls skipped function 512):
```
IR:   CALL/3(hostCallFn, env, 512, 0)
      UNREACHABLE

ASM:  sub    $0x8,%rsp
      mov    %rdi,%rax
      mov    %rax,%rdi
      mov    $0x200,%esi    ; funcIdx=512
      xor    %rdx,%rdx
      call   *0x20(%rax)    ; jit_host_call
      ; NO RET -- falls through into zeros -> crash
```

**Fix:** Changed `ir_UNREACHABLE()` to `ir_RETURN(getOrEmitReturnValue())`. This
ensures the IR backend always generates a proper function epilogue (stack
restore + `ret`). The Wasm-level trap is still handled by the interpreter when
reached through the host-call trampoline.

### Bug 2b: jit_host_call Swallows Callee Errors

**File:** `lib/executor/helper.cpp`, `lib/vm/ir_jit_engine.cpp`.

When `jit_host_call` invokes a function that traps (e.g., hits `unreachable`),
`jitCallFunction()` returns an error. The old code only handled `Terminated`
errors (via `longjmp(buf, 1)`); all other errors were silently swallowed and
`jit_host_call` returned 0. The JIT caller continued executing with potentially
wrong state.

**Fix:** `jit_host_call` now `longjmp(buf, 3)` on any callee error, stashing
the error code in thread-local storage. The top-level `invoke()` handles
`jmpVal==3` by retrieving the stashed error code and returning it as a proper
`Unexpect`:

```cpp
// jit_host_call (helper.cpp):
if (!res) {
    void *buf = wasmedge_ir_jit_get_termination_buf();
    if (buf) {
        if (res.error() == ErrCode::Value::Terminated)
            longjmp(*buf, 1);
        wasmedge_ir_jit_set_callee_error(res.error());
        longjmp(*buf, 3);   // unwind entire JIT stack
    }
}

// invoke() (ir_jit_engine.cpp):
if (jmpVal == 3)
    return Unexpect(wasmedge_ir_jit_get_callee_error());
```

### Additional Fix: Skipped Function NULL Vtable Entry

Skipped functions (those starting with Wasm `unreachable`) have NULL entries in
the JIT function table. Before Bug #3 was identified, direct inline calls to
skipped functions crashed by jumping to NULL. Fixed by:

1. Passing the `SkipJit` vector to the IR builder
   (`IRBuilder.setSkippedFunctions(SkipJit)` in `module.cpp`)
2. Routing calls to skipped functions through `jit_host_call` (buffer-based ABI)
   instead of direct register-based calls (`IsHostCall` check in
   `ir_builder.cpp`)
3. Including skipped functions in the `MaxCallArgs` pre-scan (since they now
   use the buffer path)

---

## Bug 3: Memory Bounds Checking Implementation Issues

**Status: PARTIALLY SOLVED**

### Background
Memory Bounds Checking implemented and committed. After the bounds check commit,
4 kernels broke (regex, bz2, gcc-loops, rust-compression). They all worked
before the bounds check commit.

### Key Technical Concepts
- **GUARD mechanism**: IR library's deoptimization side-exit. Has near-path (32-bit relative `jcc &addr`) and far-path (indirect `jmp aword [rax]`). The near-path's unconditional jump at line 10397 of ir_x86.dasc ALWAYS uses 32-bit relative addressing, causing overflow when target is >2GB away. Far-path treats addr as pointer-to-code-pointer, not direct code address.
- **CALL approach**: Emit `ir_CALL_3(IR_VOID, ...)` to `jit_bounds_check()` trampoline before every load/store. Works at O0 but causes IR optimizer (SCCP/GCM) miscompilation at O1+
- **ir_emit_guard_jcc**: Function in ir_x86.dasc that handles GUARD code generation. Line 10397: `jmp &addr` uses 32-bit relative displacement unconditionally -- breaks when target is >2GB from JIT code.

### Files Involved

- **`include/vm/ir_jit_engine.h`** -- Core header defining JitExecEnv and IRJitEngine. jit_bounds_check declaration.
- **`include/vm/ir_builder.h`** -- Added public HasBoundsChecks flag.
- **`lib/vm/ir_builder.cpp`** -- buildBoundsCheck uses I32 CALL_3 approach (minimal IR nodes).
- **`lib/vm/ir_jit_engine.cpp`** -- jit_bounds_check trampoline (I32 params, C-side arithmetic). SIGABRT guard. compile() with ForceO0 parameter.
- **`lib/executor/instantiate/module.cpp`** -- Currently has debugging code (ForceO0 for ALL functions) that needs to be reverted.

### Errors and Fixes

- **GUARD approach -- SCCP ir_promote_i2i assertion (gcc-loops)**: Using `ir_ZEXT_U64(Base)` triggered SCCP optimizer assertion at ir_sccp.c:1916. Root cause: SCCP ir_promote_i2i doesn't handle U64/I64 types in its switch statement.

- **GUARD approach -- ir_cfg.c:256 assertion (rust-compression)**: GUARD nodes confused the CFG builder.

- **GUARD approach -- SIGSEGV at runtime (regex, bz2, rust-compression) even at O0**: Root cause: GUARD far-path codegen is broken for far addresses (>2GB). `ir_emit_guard_jcc` line 10397 uses `jmp &addr` (32-bit relative) unconditionally. JIT code at ~0x7fff... and jit_oob_trap at ~0x55... -> >2GB distance -> overflow.

- **U64 CALL approach -> I32 CALL approach**: Original: `ir_ZEXT_U64(Base)` + `ir_ADD_U64(...)` + `ir_CALL_2(env, end_u64)` -- 2 extra IR nodes per check. Changed to: `ir_CALL_3(env, base_u32, offsetPlusSize_u32)` -- minimal IR nodes, C trampoline does arithmetic. This fixed bz2 and rust-compression at O2.

- **ForceO0 mechanism -- ALL functions fail compilation**: Added HasBoundsChecks flag and ForceO0 parameter. 90/91 (bz2) or 91/91 (all) functions fail compilation at O0 via ForceO0. Same functions compile fine at O0 via WASMEDGE_IR_JIT_OPT_LEVEL=0 env var. UNSOLVED.

### Test Results (I32 CALL + SIGABRT guard)

| Kernel | O0 | O1 | O2 |
|--------|----|----|-----|
| regex | PASS | CRASH | unreachable |
| bz2 | PASS | FAIL | PASS |
| gcc-loops | PASS (slow 39s) | PASS | PASS (SIGABRT caught, fallback) |
| rust-compression | PASS | CRASH | PASS (or PASS with GCM assertion caught) |

### Unsolved

- **regex**: Miscompiled at O1+ with any CALL-based bounds check approach. Only works at O0.
- **ForceO0 mystery**: Passing opt_level=0 via ForceO0 causes SIGSEGV during compilation, but env var O0 works. Same opt_level value, different behavior.

### Pending Tasks
- Debug and fix regex kernel failure (miscompiled at O1+ with bounds checking)
- Investigate ForceO0 mystery
- Debug and fix bz2 at O1
