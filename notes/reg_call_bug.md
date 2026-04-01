# IR Library O0 Register Clobbering Bug — Root Cause and Fix

## Summary

At O0 (no optimization), calls with **>6 integer arguments** produced incorrect
results because the IR library's call-argument emission clobbered a register
argument with a stack argument.  Four Sightglass kernels failed at O0:
`blake3-scalar`, `gcc-loops`, `pulldown-cmark`, `shootout-ackermann`.

## Symptoms

`shootout-ackermann` output: `M = 0 and N = 0` (expected `M = 3 and N = 7`).

The string parser function returned 0 instead of the parsed integer. Tracing
the call chain: `main → string_parser → func34 → func28`. Function 28 (IR
dump index 016) contained a **CALL/9** (9 arguments, 3 of which spill to the
stack on SysV x86-64).

## Root Cause

**File:** `thirdparty/ir/ir_x86.dasc`, function `ir_emit_arguments`, pass 3.

The IR library emits call arguments in three passes:

1. **Pass 1** — move register-held values to stack slots (or defer).
2. **Pass 2** — `ir_parallel_copy` for register-to-register moves.
3. **Pass 3** — load remaining values from spill slots into registers
   (for register args) or onto the stack (for stack args).

At O0, `ir_allocate_unique_spill_slots` assigns every value a unique spill
slot and a *temporary register* for loading.  The CALL instruction's
constraint system (`ir_get_target_constraints`) allocates a **tmp register at
slot 1** for indirect calls:

```c
// ir_x86.dasc line 1554-1556 (inside case IR_CALL):
constraints->tmp_regs[n] = IR_TMP_REG(1, IR_ADDR, IR_LOAD_SUB_REF, IR_USE_SUB_REF);
```

`ir_emit_call` then passes `ctx->regs[def][1]` as the `tmp_reg` argument to
`ir_emit_arguments`.  At O0, the simple allocator picked **RCX** for this slot
(the first available scratch register after the parameter registers were
assigned to other constraints).

In pass 3, **register arguments** are loaded into their destination registers
(RDI, RSI, RDX, RCX, R8, R9) from spill slots.  Then **stack arguments** use
`tmp_reg` to load from spill slots and store to the outgoing stack frame.
Since `tmp_reg` was RCX, the stack-argument setup **clobbered the 4th register
argument** (RCX) that was already loaded earlier in the same pass:

```asm
;; Pass 3 register arg setup:
mov    0x2c(%rsp),  %ecx        ; arg3 = p1 (correct value)
mov    0x170(%rsp), %r8d        ; arg4 = d_94
mov    0x190(%rsp), %r9         ; arg5 = d_99

;; Pass 3 stack arg setup — CLOBBERS ECX:
mov    0x188(%rsp), %rcx        ; tmp_reg=RCX, loading d_98 for stack
mov    %rcx,        (%rsp)      ; → stack arg 0
mov    0x198(%rsp), %ecx        ; loading d_100 for stack
mov    %ecx,        0x8(%rsp)   ; → stack arg 1
mov    0x19c(%rsp), %ecx        ; loading d_101 for stack
mov    %ecx,        0x10(%rsp)  ; → stack arg 2
call   *0x1b0(%rsp)             ; ECX = d_101, should be p1!
```

The callee received the wrong 4th argument, computed incorrectly, and
returned 0.

## Fix

**File:** `thirdparty/ir/ir_x86.dasc` (and generated `ir_emit_x86.h`),
function `ir_emit_arguments`, pass 3 stack-argument handling.

Introduced a dedicated `stk_tmp = IR_REG_RAX` for loading stack arguments
instead of reusing `tmp_reg`.  RAX is never a parameter register on any
x86-64 calling convention (SysV or Windows), so it cannot conflict with
register arguments loaded earlier in pass 3.

```c
// Pass 3, stack arg handling (dst_reg == IR_REG_NONE):
ir_reg stk_tmp = IR_REG_RAX;   // ← NEW: safe scratch for stack args

if (IR_IS_TYPE_INT(type)) {
    if (IR_IS_CONST_REF(arg)) {
        ir_emit_store_mem_int_const(ctx, type, mem, arg, stk_tmp, 1);
    } else if (src_reg == IR_REG_NONE) {
        ir_emit_load(ctx, type, stk_tmp, arg);
        ir_emit_store_mem_int(ctx, type, mem, stk_tmp);
    } else if (IR_REG_SPILLED(src_reg)) {
        ir_emit_load(ctx, type, stk_tmp, arg);
        ir_emit_store_mem_int(ctx, type, mem, stk_tmp);
    }
}
```

The original `tmp_reg` is preserved for pass 2 (`ir_parallel_copy`) where it
is used as a swap register for resolving register-to-register copy cycles.
This avoids any regression at O1/O2.

## Why the fix is safe

| Property | Explanation |
|----------|-------------|
| RAX not a param reg | SysV: RDI RSI RDX RCX R8 R9.  Windows: RCX RDX R8 R9.  RAX is absent from both. |
| RAX is scratch | RAX is caller-saved, so it is available as a temporary before the call. |
| No conflict with return value | The call hasn't happened yet; RAX will be overwritten by the return value. |
| Pass 2 unaffected | `ir_parallel_copy` still uses the original `tmp_reg`, preserving O1/O2 behavior. |

## Test results after fix

| Opt level | Result |
|-----------|--------|
| O0 | 37/38 Sightglass kernels pass (1 timeout: shootout-base64, expected for unoptimized code) |
| O2 | 36/38 pass (2 pre-existing optimizer crashes: rust-compression, shootout-ed25519 — both also crash at O1, unrelated to this fix) |
