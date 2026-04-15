# Calling Convention for IR JIT

## Overview

The WasmEdge IR JIT uses the **dstogov/ir** library to compile Wasm functions
to native x86-64 machine code.  This document describes the calling convention
evolution: from buffer-based, to register-based, and the decision to keep
buffer-based as the default after benchmarking showed register-based is
execution-neutral with unpredictable per-kernel variance from LSRA sensitivity.

### Buffer-based (current default)

Every JIT-compiled function has the uniform signature:

```c
ret func(JitExecEnv *env, uint64_t *args);
```

Arguments are marshalled into a `uint64_t[]` buffer by the caller, and the
callee loads them with `ir_LOAD` from buffer offsets.  Direct JIT-to-JIT calls
use inline `ir_CALL_N` with a 2-param proto; host calls and `call_indirect`
go through their respective C trampolines.

### Register-based (implemented, disabled)

Each JIT-compiled function declares its Wasm parameters as individual
`ir_PARAM` nodes with native IR types.  The IR library assigns SysV ABI
registers automatically.  Direct JIT-to-JIT calls use `ir_CALL_N` with a
typed `ir_proto`, passing arguments directly in registers.

```
ret func(JitExecEnv *env, arg0, arg1, ...)
         ^^^ rdi           ^^^ rsi/xmm0, rdx/xmm1, ...
```

**Why it is disabled:** see "Performance Analysis" section below.

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

## Performance Analysis

Benchmarked all Sightglass kernels at O2, 3-run medians:
- **ir_jit**: baseline (buffer-based, `kRegCallMaxParams = 0`)
- **reg_call**: register-based (`kRegCallMaxParams = 100`)
- Excluded: spidermonkey, tinygo, rust-compression (crashes on reg_call),
  richards (no output), shootout-ackermann (high variance)

### Aggregate

| Metric | ir_jit (µs) | reg_call (µs) | Speedup |
|--------|-------------|---------------|---------|
| Exec   | 9,194,428   | 9,183,915     | 1.00×   |
| Total (compile+exec) | 12,849,494 | 12,000,130 | 1.07× |

Execution time is flat in aggregate. The 7% total-time improvement is
driven by compile-time reductions.

### Per-kernel exec results

**Exec improvements (reg_call faster):**

| Kernel | ir_jit (µs) | reg_call (µs) | Speedup |
|--------|-------------|---------------|---------|
| shootout-ed25519 | 1,731,933 | 1,485,257 | 1.17× |
| blind-sig | 46,552 | 41,537 | 1.12× |
| shootout-ctype | 140,291 | 132,839 | 1.06× |

**Exec regressions (reg_call slower):**

| Kernel | ir_jit (µs) | reg_call (µs) | Ratio |
|--------|-------------|---------------|-------|
| shootout-sieve | 140,237 | 173,047 | 0.81× |
| rust-html-rewriter | 10,398 | 12,753 | 0.82× |
| hashset | 29,157 | 34,811 | 0.84× |
| shootout-ratelimit | 173,164 | 201,139 | 0.86× |
| shootout-minicsv | 1,179,992 | 1,341,543 | 0.88× |
| shootout-fib2 | 561,523 | 594,979 | 0.94× |

Regressions are consistent across all 3 runs (not noise).

**Compile-time improvements:**

| Kernel | ir_jit (µs) | reg_call (µs) | Speedup |
|--------|-------------|---------------|---------|
| hashset | 1,018,675 | 246,490 | 4.13× |
| shootout-ed25519 | 143,376 | 98,651 | 1.45× |

The 7% total-time win is almost entirely from hashset's 4.13× compile-time
reduction (772 ms saved). Without hashset the total speedup is ~1.01×.

### What register-based calling changes in the IR graph

Register-based calling modifies the IR graph in two ways:

**1. Fewer IR nodes per call site.** Buffer-based emits N `ir_STORE` +
N `ir_ADD_A` + 1 `ir_CALL_2` per direct JIT call. Register-based emits
1 `ir_CALL_(N+1)`. This explains the compile-time improvements: fewer
nodes → faster register allocation and optimization.

**2. Different SSA numbering.** SSA numbers are assigned sequentially as
nodes are emitted. Fewer nodes at call sites shift all subsequent SSA
numbers. The LSRA processes live intervals in start-point order (SSA
number), so a small shift propagates through the entire function, changing
which intervals compete for which registers at each allocation step.

### How `ir_PARAM` interacts with the register allocator

The IR library's register allocator (`ir_emit.c:ir_get_param_reg`,
`ir_x86.dasc:1667`) assigns each `ir_PARAM` node a **fixed physical
register constraint** — the SysV ABI register it arrives in (RDI, RSI,
RDX, RCX, R8, R9 for integer params; XMM0–XMM7 for FP). The LSRA must
keep that ABI register reserved from function entry until the value is
copied to its allocated register:

```c
case IR_PARAM:
    constraints->def_reg = ir_get_param_reg(ctx, ref);  // e.g., RSI
    // ...
    // Fixed live range blocks the ABI register from start to def_pos:
    ir_add_fixed_live_range(ctx, reg, start, def_pos);
```

`ir_LOAD` values from the buffer have **no fixed-register constraint** —
the LSRA can target the load directly into any register (including
callee-saved).

If a param lives across a call, the LSRA moves it to a callee-saved
register in both cases. But `ir_PARAM` additionally blocks the ABI
register during the copy window, and these extra fixed-range constraints
interact with the LSRA's allocation decisions for all other intervals.

### The direct savings are modest

Per direct JIT call with N args, register-based eliminates:
- **Caller:** N `ir_STORE` instructions (marshal args into buffer)
- **Callee prologue:** N `ir_LOAD` instructions (read args from buffer)

It replaces them with register-to-register moves (value → ABI reg at
caller + ABI reg → allocated reg at callee). Register moves are ~1 cycle;
L1 loads/stores are 1–4 cycles. For a call with 3 args: savings of
~6–18 cycles per call — negligible relative to overall function execution.

### `call_indirect` does not benefit

`call_indirect` always goes through the `jit_call_indirect` C trampoline
(buffer-based). Even with `kRegCallMaxParams = 100`:

1. Caller stores args to `SharedCallArgs` buffer (same cost)
2. Calls `jit_call_indirect(env, tableIdx, elemIdx, typeIdx, args, retCode)`
3. Trampoline does Wasm type-check, then calls `irJitInvokeNative`
4. `irJitInvokeNative` dispatches through the autogenerated switch cascade
   (~21K paths) to call the function with the correct register signature

The buffer-based path (`irJitInvokeNativeBuffer`) is simpler: a single
`reinterpret_cast` and call.  So `call_indirect` to JIT targets is
**strictly more expensive** with register-based targets due to the
autogenerated dispatcher overhead.

### Why specific kernels regress

The dominant effect is **LSRA sensitivity to SSA numbering**, not
callee-saved register pressure per se. Evidence:

**shootout-sieve (−19%):** The hot function `__original_main` has **0
wasm params** (type `() → i32`). With `kRegCallMaxParams = 0`, `0 ≤ 0`
→ register-based path. With `kRegCallMaxParams = 100`, same. The callee
prologue is identical on both branches. The only structural difference is
at the `memset` call site: buffer-based emits 3 `ir_STORE` + `ir_CALL_2`,
register-based emits `ir_CALL_4`. This ~6-node SSA shift propagates to the
inner sieve loop (8193 iterations × 17000 outer iterations), where the LSRA
makes different allocation decisions for loop-invariant values like
`MemoryBase`. Since the inner loop has no function calls, callee-saved vs
caller-saved is irrelevant — but the LSRA's heuristic register choices
(which register for which value) change due to the shifted processing order.

**shootout-fib2 (−6%):** Recursive with 1 param. The two recursive call
sites emit different IR structure, shifting SSA numbering for all
subsequent nodes in the function.

**shootout-ed25519 (+17%):** 1402 direct calls, minimal `call_indirect`
(3). The LSRA perturbation from the restructured IR graph happens to
produce better allocation for the large computation functions in this
module. The same mechanism that causes regressions in other kernels
produces improvements here — it is not predictable from the calling
convention alone.

### LSRA sensitivity to SSA ordering

The dstogov/ir LSRA is a standard linear-scan allocator that processes
live intervals sorted by start point. When two intervals compete for the
same physical register, the one processed first (earlier SSA number) gets
priority. Small changes in SSA numbering — from adding or removing a few
IR nodes — can flip specific allocation decisions:

- A loop-invariant value that got a callee-saved register may instead spill
- Two values may swap register assignments, changing move/shuffle code at
  call sites
- The order in which intervals are evicted changes

This sensitivity is a well-known property of linear-scan allocators.
Graph-coloring allocators have similar issues with node ordering. The
effect is that any IR graph restructuring — regardless of whether it is
"better" structurally — introduces allocation noise that can dominate
small signals like eliminated buffer marshalling.

The prologue emission order also exploits this sensitivity: in the current
code, wasm param loads are emitted *before* env field loads. This gives
wasm params earlier SSA numbers and higher LSRA priority for callee-saved
registers, which benefits recursive/call-heavy functions where params must
survive across calls. This ordering was validated by isolated benchmarks
(fib2 −12%, ackermann −9% improvement over the reverse order).

### Conclusion

Register-based calling is execution-neutral in aggregate (1.00×). The
direct savings from eliminating buffer marshalling are small (~6–18 cycles
per call with 3 args). These savings are drowned out by LSRA allocation
noise from the changed IR graph structure, which produces unpredictable
per-kernel variance (±19%). The 7% total-time improvement is almost
entirely from compile-time reduction (fewer IR nodes → faster RA), not
execution improvement.

The threshold mechanism is retained in code for future use. Two paths to
realizing the register-based benefit:

1. **Improve the IR LSRA** to be less sensitive to SSA ordering (e.g.,
   priority-based allocation, better heuristic tie-breaking).
2. **Combine with other optimizations** (e.g., inlined `call_indirect`
   hot path) that reduce the relative weight of call overhead, making the
   buffer savings more visible.

---

## Known Limitations

1. **`kRegCallMaxParams = 0`:** Register-based calling is fully implemented
   but disabled. The threshold can be raised if the IR LSRA gains better
   stability with respect to SSA ordering perturbations.

2. **Arity > 6 for `irJitInvokeNative`:** The register-based host-to-JIT
   entry aborts for functions with >6 Wasm parameters. Not an issue with
   `kRegCallMaxParams = 0` since `irJitInvokeNativeBuffer` handles all
   arities.

3. **Single return value:** Only the first Wasm return type is handled.
   Multi-value returns would require struct-return ABI or additional
   convention.

4. **`DirectOrHostFn` in JitExecEnv:** Field still exists in the struct and
   the prologue still loads it (`ir_builder.cpp:247`), but it is unused
   since commit c63e38f7 (inline direct calls). The struct field is retained
   for ABI stability; the prologue load should be removed (it wastes an IR
   node, though O2 DCE may eliminate it).

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

### Note on benchmark confounding

The `reg_call` branch includes bug fix `7873c3b` (duplicate-arg
spill-reload) in `thirdparty/ir`, which the `ir_jit` baseline does not.
While this fix targets a rare case (same variable as two operands of a
single CALL), its presence changes RA code paths. Ideally the benchmark
should be re-run with identical `thirdparty/ir` on both branches to fully
isolate the calling convention effect.
