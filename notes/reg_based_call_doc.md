# Calling Convention for IR JIT

## Overview

The WasmEdge IR JIT uses the **dstogov/ir** library to compile Wasm functions
to native x86-64 machine code.  This document describes the calling convention
evolution: from buffer-based, to register-based, and the final decision to
revert to buffer-based after performance analysis showed register-based hurts
more than it helps.

### Buffer-based (current)

Every JIT-compiled function has the uniform signature:

```c
ret func(JitExecEnv *env, uint64_t *args);
```

Arguments are marshalled into a `uint64_t[]` buffer by the caller, and the
callee loads them with `ir_LOAD` from buffer offsets.  Direct JIT-to-JIT calls
use inline `ir_CALL_N` with a 2-param proto; host calls and `call_indirect`
go through their respective C trampolines.

### Register-based (attempted, reverted)

Each JIT-compiled function declared its Wasm parameters as individual
`ir_PARAM` nodes with native IR types.  The IR library assigned SysV ABI
registers automatically.  Direct JIT-to-JIT calls used `ir_CALL_N` with a
typed `ir_proto`, passing arguments directly in registers.

```
ret func(JitExecEnv *env, arg0, arg1, ...)
         ^^^ rdi           ^^^ rsi/xmm0, rdx/xmm1, ...
```

**Why it was reverted:** see "Performance Analysis" section below.

### Hybrid threshold mechanism (in code, threshold = 0)

The code supports a compile-time threshold `kRegCallMaxParams` (in
`ir_jit_reg_invoke.h`). Functions with ≤threshold wasm params use register-
based; functions with more use buffer-based. Currently set to **0**, meaning
all JIT-to-JIT calls use buffer-based. Can be raised if the IR register
allocator improves.

---

## Architecture

### Call categories

| Call kind | Path | ABI |
|-----------|------|-----|
| Direct call to JIT function (≤threshold params) | Inline `ir_CALL_N` with typed proto | Register (SysV) |
| Direct call to JIT function (>threshold params) | Inline `ir_CALL_N` with 2-param proto | Buffer (`uint64_t[]`) |
| Direct call to import or skipped function | `jit_host_call` trampoline | Buffer (`uint64_t[]`) |
| `call_indirect` | `jit_call_indirect` trampoline | Buffer (`uint64_t[]`) |
| Host-to-JIT entry (`IRJitEngine::invoke`) | `irJitInvokeNative` or `irJitInvokeNativeBuffer` | Depends on arity |

With `kRegCallMaxParams = 0`, all JIT-to-JIT calls use the buffer path.

### Component diagram

```
                  Host / Interpreter
                         |
                  IRJitEngine::invoke()
                         |
              ┌──────────┴──────────┐
              │ ≤threshold params   │ >threshold params
              │                     │
       irJitInvokeNative()   irJitInvokeNativeBuffer()
              │                     │
              └──────────┬──────────┘
                         |
                   JIT function A
                    /          \
          direct call        host call
          (inline)           (trampoline)
              |                  |
        JIT function B    jit_host_call()
                                 |
                          WasmEdge Executor
```

---

## Callee Side: Function Prologue

**File:** `ir_builder.cpp`, `WasmToIRBuilder::initialize()`

The prologue switches on `kRegCallMaxParams`:

**Register-based** (ParamTypes.size() ≤ threshold):

```cpp
EnvPtr = ir_PARAM(IR_ADDR, "exec_env", 1);
for (uint32_t i = 0; i < ParamTypes.size(); ++i) {
    Locals[i] = ir_PARAM(irType, name, i + 2);
}
```

**Buffer-based** (ParamTypes.size() > threshold, or threshold = 0):

```cpp
EnvPtr = ir_PARAM(IR_ADDR, "exec_env", 1);
ArgsPtr = ir_PARAM(IR_ADDR, "args", 2);
for (uint32_t i = 0; i < ParamTypes.size(); ++i) {
    Locals[i] = ir_LOAD_XXX(ArgsPtr + i * 8);
}
```

The IR library assigns registers per the SysV x86-64 ABI:
- Integer/pointer args: RDI, RSI, RDX, RCX, R8, R9 (then stack)
- FP args: XMM0–XMM7 (then stack)

Since `exec_env` always consumes RDI (integer slot 0), the buffer pointer
or first Wasm parameter starts at RSI.

### Return type

`Ctx.ret_type` is set to the Wasm function's native return type (IR_I32,
IR_I64, IR_FLOAT, IR_DOUBLE, or IR_VOID).  The IR library generates
the appropriate `ret` instruction, placing integer results in RAX and
FP results in XMM0.

---

## Caller Side: Direct Calls

**File:** `ir_builder.cpp`, `WasmToIRBuilder::visitCall()`

Three call paths in `visitCall()`:

1. **Host call** (import or skipped function): marshal args into
   `SharedCallArgs` buffer, call `jit_host_call(env, funcIdx, args)`.

2. **High-arity JIT call** (>threshold params): marshal args into
   `SharedCallArgs` buffer, call callee directly as `func(env, args)`.

3. **Low-arity JIT call** (≤threshold params): pass args in registers
   via typed `ir_proto` with per-callee-type signature.

With `kRegCallMaxParams = 0`, path 2 handles all JIT-to-JIT calls.

```cpp
// High-arity / buffer path:
ir_ref BufProtoParams[2] = {IR_ADDR, IR_ADDR};
ir_ref BufProto = ir_proto(ctx, IR_FASTCALL_FUNC, RetType, 2, BufProtoParams);
ir_ref CallResult = ir_CALL_N(RetType, TypedFuncPtr, 2, {EnvPtrVal, CalleeArgs});

// Low-arity / register path:
ProtoParams[0] = IR_ADDR;  // exec_env
for (i = 0; i < NumArgs; ++i)
    ProtoParams[i + 1] = wasmTypeToIRType(ParamTypes[i]);
ir_ref CallResult = ir_CALL_N(RetType, TypedFuncPtr, TotalParams, DirectArgs);
```

### Return type convention

The caller proto's return type is:
- `IR_FLOAT` / `IR_DOUBLE` for FP returns (XMM0)
- `IR_I64` for integer returns *and* void (RAX; the IR x86 backend asserts
  on `IR_VOID` in the address-fusion pass at O2)

For I32-returning callees, the result is declared as I64 in the proto and
truncated with `ir_TRUNC_I32` after the call.

---

## Host-to-JIT Entry

**Files:** `ir_jit_reg_invoke.h`, `ir_jit_reg_invoke.cpp`,
`ir_jit_reg_invoke_autogen.cpp`

When the WasmEdge runtime calls a JIT function from C++ (via
`IRJitEngine::invoke`), it selects the invoke path based on arity:

### `irJitInvokeNative` (register-based, ≤threshold params)

Two-level dispatch:

1. **`irJitInvokeNative`** (hand-written): checks arity (0–6), dispatches
   to the autogenerated function based on whether the return type is void.
   Aborts for arity > 6.

2. **`irJitInvokeNativeAutogen`** (generated by
   `gen_ir_jit_reg_invoke_autogen.py`): switches on return type, then on
   arity, then on each parameter type combination.  For each combination,
   it `reinterpret_cast`s the function pointer to the exact C signature and
   calls it.  Total: ~21,844 dispatch paths.

### `irJitInvokeNativeBuffer` (buffer-based, >threshold params)

Simple dispatch: casts the function pointer to `ret (*)(JitExecEnv*, uint64_t*)`
with the appropriate return type (void, uint64_t, float, or double) and calls
it directly. No autogeneration needed — a single function handles all arities.

### Selection in `invoke()`

```cpp
bool useBuffer = (ParamTypes.size() > kRegCallMaxParams);
if (useBuffer)
    Raw = detail::irJitInvokeNativeBuffer(NativeFunc, &Env, FuncType, ArgsData);
else
    Raw = detail::irJitInvokeNative(NativeFunc, &Env, FuncType, ArgsData);
```

With `kRegCallMaxParams = 0`, all calls go through `irJitInvokeNativeBuffer`.

---

## JitExecEnv Structure

**File:** `ir_jit_engine.h`

```
Offset  Field               Type           Description
──────  ──────────────────  ────────────   ─────────────────────────────────
 0x00   FuncTable           void**         Pointers to JIT native code (by funcIdx)
 0x08   FuncTableSize       uint32_t       Number of entries in FuncTable
 0x0C   _pad                uint32_t       Alignment padding
 0x10   GlobalBase          void*          Base of Wasm global variables
 0x18   MemoryBase          void*          Base of linear memory (memory 0)
 0x20   HostCallFn          void*          → jit_host_call
 0x28   DirectOrHostFn      void*          → jit_direct_or_host (legacy, unused by inline calls)
 0x30   MemoryGrowFn        void*          → jit_memory_grow
 0x38   MemorySizeFn        void*          → jit_memory_size
 0x40   CallIndirectFn      void*          → jit_call_indirect
 0x48   MemorySizeBytes     uint64_t       Current memory size (for bounds checking)
 0x50   RefResultBuf[2]     uint64_t[2]    Scratch buffer for ref-return helpers
```

`JitExecEnv *` is always the first argument (RDI) to every JIT function.
The function prologue loads needed fields from it (FuncTable, GlobalBase,
MemoryBase, trampoline pointers) into IR temporaries.

---

## Trampolines

### `jit_host_call(env, funcIdx, args)`

Dispatches calls to import functions (host functions) through the WasmEdge
executor.  Marshals `uint64_t[]` args into `ValVariant` parameters,
calls `Executor::jitCallFunction`, and packs the return value back to
`uint64_t`.

Also handles the fast path for `call_indirect` targets that are JIT-compiled:
when `funcIdx` has bit 31 set (`0x80000000 | tableSlot`), it resolves the
funcref from the table and calls `irJitInvokeNative` directly if the target
is a JIT function.

### `jit_call_indirect(env, tableIdx, elemIdx, typeIdx, args, retTypeCode)`

Delegates to `Executor::jitCallIndirect` which performs the Wasm-specified
type check (matching against the module's type section) before dispatching.
If the target is a JIT function, it uses register-based ABI via
`irJitInvokeNative`; otherwise falls back to the interpreter.

### `jit_memory_grow(env, nPages)` / `jit_memory_size(env)`

Thin wrappers around `MemoryInstance::growPage` and `getPageSize`.
`jit_memory_grow` updates `env->MemoryBase` and `env->MemorySizeBytes`
after a successful grow (the buffer may have been reallocated).

---

## IR Dump Example

A function `f(i32, i32) → i32` that calls another function, with buffer-
based ABI (`kRegCallMaxParams = 0`):

```
l_1 = START(l_N);
uintptr_t d_2 = PARAM(l_1, "exec_env", 1);   // rdi
uintptr_t d_3 = PARAM(l_1, "args", 2);        // rsi (buffer ptr, dies early)

// Load wasm params from buffer
int32_t d_4, l_4 = LOAD(l_1, d_3);            // args[0] → p0
uintptr_t d_5 = ADD(d_3, 0x8);
int32_t d_6, l_6 = LOAD(l_4, d_5);            // args[1] → p1
// d_3 (args ptr) is dead after this point — callee-saved reg freed

// Load FuncTable from exec_env
uintptr_t d_7 = ADD(d_2, 0);
uintptr_t d_8, l_8 = LOAD(l_6, d_7);

// Load callee pointer from FuncTable[targetIdx]
uintptr_t d_9 = ADD(d_8, targetIdx * 8);
uintptr_t d_10, l_10 = LOAD(l_8, d_9);

// Marshal args to SharedCallArgs buffer, call as func(env, buf)
STORE(l_10, SharedCallArgs, d_4);              // args[0] = p0
STORE(l_10, SharedCallArgs+8, d_6);            // args[1] = p1
uintptr_t d_11 = PROTO(d_10, func(uintptr_t, uintptr_t): int64_t __fastcall);
int64_t d_12, l_12 = CALL/2(l_10, d_11, d_2, SharedCallArgs);

int32_t d_13 = TRUNC(d_12);
l_N = RETURN(l_12, d_13);
```

---

## Performance Analysis: Why Register-Based Calling Hurts

Benchmarked all Sightglass kernels at O2, 3-run medians, comparing:
- **BEFORE**: commit 8ead9c32 (pre-reg-call, all buffer-based)
- **AllBuf**: current code with `kRegCallMaxParams = 0` (all buffer)
- **Hybrid**: current code with `kRegCallMaxParams = 3`

### Results (selected kernels, WorkTime µs)

| Kernel | BEFORE | AllBuf | Hybrid | AllBuf% | Hybrid% |
|---|---|---|---|---|---|
| shootout-ackermann | 657 | **526** | 632 | **-19.9%** | -3.7% |
| shootout-sieve | 176K | **139K** | 172K | **-21.2%** | -2.1% |
| shootout-fib2 | 647K | **574K** | 612K | **-11.3%** | -5.4% |
| hashset | 30K | 30K | **27K** | -2.1% | **-11.5%** |
| richards | 23.2M | 25.9M | 28.7M | +11.7% | +23.6% |
| ratelimit | 174K | 194K | 210K | +11.9% | +20.9% |
| blake3 | 60 | 61 | 102 | +1.7% | +69.9% |

AllBuf wins overall: 9 improved, 13 neutral, 11 regressed vs BEFORE.
Hybrid is strictly worse than AllBuf on most kernels.

### Root cause: `ir_PARAM` liveness pins callee-saved registers

The IR LSRA assigns each `ir_PARAM` to a **callee-saved register** (rbx,
rbp, r12–r15) and holds it for the param's full live range. x86-64 has
6 callee-saved GPRs total.

**Buffer-based** — `func(env, args_buf)`:
```
env  = ir_PARAM("exec_env", 1)  →  rbx  (live entire function)
buf  = ir_PARAM("args", 2)      →  rbp  (dies after prologue loads)
p0   = ir_LOAD(buf + 0)         →  regular IR value
p1   = ir_LOAD(buf + 8)         →  regular IR value
// buf is dead → rbp freed → 5 callee-saved regs available for body
```

The buffer pointer `args` dies immediately after the last parameter load.
Its callee-saved register is released. The loaded parameters are fresh IR
values — the allocator assigns registers based on actual liveness (callee-
saved if needed across calls, caller-saved if short-lived).

**Register-based** — `func(env, p0, p1, p2)`:
```
env = ir_PARAM("exec_env", 1)  →  rbx  (live entire function)
p0  = ir_PARAM("p0", 2)        →  rbp  (live until last use of p0)
p1  = ir_PARAM("p1", 3)        →  r12  (live until last use of p1)
p2  = ir_PARAM("p2", 4)        →  r13  (live until last use of p2)
// 2 callee-saved regs available for body
```

Each `ir_PARAM` IS the value used throughout the function. Wasm params are
typically live for the entire function (loop counters, base addresses), so
they pin callee-saved registers permanently.

### Why callee-saved registers matter

In a function with N call sites, **callee-saved registers survive every
call for free**. Values in caller-saved registers must be spilled before
and reloaded after each call. The loop-invariant values from `JitExecEnv`
— `FuncTablePtr`, `MemoryBase`, `GlobalBasePtr` — are used at every memory
access and call site.

Buffer-based with 0 wasm params uses 1 callee-saved (env), leaving 5 for:
```
FuncTablePtr → r12 (survives all N calls)
MemoryBase   → r13 (survives all N calls)
GlobalBasePtr→ r14 (survives all N calls)
// + 2 more for hot locals
```

Register-based with 3 wasm params uses 4 callee-saved, leaving only 2:
```
FuncTablePtr → r14 (survives all N calls)
MemoryBase   → r15 (survives all N calls)
GlobalBasePtr→ spill to stack, reload at every use after a call
```

For ratelimit func 043 (95 call sites, 5128 IR lines): losing 1 callee-
saved register = 95 extra reload pairs per evicted value.

### The cost asymmetry

- **Buffer overhead**: 2 instructions per parameter per call (store at
  caller + load in callee prologue). Paid once.
- **Lost callee-saved register**: 2 instructions (spill + reload) per
  call site per evicted loop-invariant value. Paid N times.

Even for functions with 1 wasm param, the buffer pointer dies early and
frees a callee-saved register that can save N reloads. The one-time buffer
marshalling cost is trivially amortized.

### Conclusion

Register-based calling is a net loss with the current IR LSRA because
`ir_PARAM` values monopolize callee-saved registers. Buffer-based calling
gives the allocator more freedom by separating the parameter-passing
mechanism (short-lived buffer pointer) from the parameter values (regular
IR values with independent liveness). The hybrid threshold mechanism is
retained in code for future use if the IR register allocator gains better
`ir_PARAM` liveness splitting.

---

## Known Limitations

1. **`kRegCallMaxParams = 0`:** Register-based calling is fully implemented
   but disabled. The threshold can be raised if the IR LSRA gains better
   param liveness management.

2. **Arity > 6 for `irJitInvokeNative`:** The register-based host-to-JIT
   entry aborts for functions with >6 Wasm parameters. Not an issue with
   `kRegCallMaxParams = 0` since `irJitInvokeNativeBuffer` handles all
   arities.

3. **Single return value:** Only the first Wasm return type is handled.
   Multi-value returns would require struct-return ABI or additional
   convention.

4. **`DirectOrHostFn` in JitExecEnv:** Loaded in the prologue but no longer
   used by inline direct calls.  Retained for ABI stability; can be removed
   in a future cleanup.

5. **Remaining O2 crash:** `rust-compression` crashes at O2 only (passes at
   O0 and O1). Root cause is a wasm memory corruption cascade from an
   O2-compiled function. See `notes/regalloc_bug.md` Remaining Issue #2.

---

## Related Bug Fixes (thirdparty/ir)

These fixes are in the IR library and remain necessary regardless of
calling convention:

1. **O0 register clobbering** (`bd5c59e`): `tmp_reg` (RCX) clobbered the
   4th register argument during stack-argument setup for calls with >6 args.
   Fix: use `IR_REG_RAX` as scratch for stack args in `ir_emit_arguments`.

2. **Duplicate-arg spill-reload** (`7873c3b`): when the same IR variable
   appeared as two operands of a CALL, the second operand skipped
   `needs_spill_reload()`, reading stale data from a clobbered caller-saved
   register. Fix: force re-evaluation when `prev_use_ref == ref`.

3. **Register allocator split logic** (`3b173e3`): upstream 2f1f018
   introduced orphaned child intervals and deferred spills in
   `ir_allocate_blocked_reg()`. Reverted to the original split/spill logic.
