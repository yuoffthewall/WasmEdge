# IR JIT O0 Bug: Unsafe Load Fusion Produces Wrong x86 Code

## Symptom

When running WasmEdge IR JIT benchmarks with `WASMEDGE_IR_JIT_OPT_LEVEL=0`, **all**
kernels that touch memory (everything except `noop`) segfault during execution.
Compilation succeeds; the crash happens in the generated JIT code at runtime.

## Example: shootout-ackermann

The IR for the entry function includes:

```
d_5, l_5 = LOAD(l_1, d_4)      ;; d_4 = exec_env + 0 (an address)
                                 ;; d_5 = *(exec_env)  (a loaded value)
...
d_22 = MUL(0x28, 0x8)           ;; = 0x140
d_23 = ADD(d_5, d_22)           ;; d_23 = *(exec_env) + 0x140
d_24 = LOAD(d_23)               ;; load function pointer from table
```

**Expected x86 at O2** (register-allocated, load fusion safe):

```asm
mov    rax, [exec_env]          ; rax = d_5 = *(exec_env)
add    rax, 0x140               ; rax = d_5 + 0x140 = d_23
mov    rax, [rax]               ; d_24
```

**Actual x86 at O0** (scratch-register-only, load fusion broken):

```asm
; d_22 = MUL(0x28, 0x8) = 0x140
mov    eax, 0x28
imul   rax, rax, 0x8            ; rax = 0x140 (d_22)
mov    [rsp+0x50], rax          ; spill d_22

; d_23 = ADD(d_5, d_22)  --  d_5's LOAD was fused into this ADD
mov    rax, [rsp+0x50]          ; rax = d_22 = 0x140  (for op1 after swap)
mov    rax, [rsp+0x08]          ; BUG: clobbers d_22! rax = d_4 (ADDRESS)
add    rax, [rax]               ; rax = d_4 + *(d_4) = exec_env + *(exec_env)
                                ; WRONG -- should be *(exec_env) + 0x140
```

The final value `exec_env + *(exec_env)` is 0xD55555C12B30 -- garbage.
The subsequent `LOAD(d_23)` dereferences it and segfaults.

## Root Cause

The bug is a chain of three interacting design assumptions in the IR x86 backend
that break at O0.

### 1. Load fusion marks the LOAD as consumed but doesn't allocate its address register

In `ir_match_insn`, when an `ADD` falls through to `IR_BINOP_INT` (as it does at O0),
`ir_match_fuse_load_commutative_int` is called. This tries to fuse one operand's
`LOAD` into the ADD as a memory operand:

```
ADD(d_5, d_22)                          ;; d_5 = LOAD(d_4)
  -> swap operands: op1=d_22, op2=d_5
  -> mark d_5's LOAD as IR_FUSED        ;; "the ADD will handle d_5's load"
  -> call ir_match_fuse_addr(d_4)       ;; try to fuse d_4 as an x86 address mode
```

When a LOAD is marked `IR_FUSED`, it is **no longer emitted as a separate instruction**.
Its register constraints (including `IR_OP2_MUST_BE_IN_REG` for the address) are never
processed by the allocator. The address register must come from somewhere else.

### 2. At O0, address ADD is `IR_BINOP_INT` not LEA -- `ir_fuse_addr` can't handle it

`ir_match_fuse_addr` checks:

```c
if (rule >= IR_LEA_FIRST && rule <= IR_LEA_LAST) {
    ctx->rules[addr_ref] = IR_FUSED | IR_SIMPLE | rule;   // fuse as x86 LEA
}
// otherwise: return without fusing -- address stays as a regular instruction
```

At O0, `IR_OPT_CODEGEN` is off, so the ADD matcher never enters the LEA branches
(they're all gated by `ctx->flags & IR_OPT_CODEGEN`). The address `d_4 = ADD(exec_env, 0)`
is matched as `IR_BINOP_INT`, not `IR_LEA_OB`. So `ir_match_fuse_addr` returns
**without fusing the address**.

Later, the emitter calls `ir_fuse_load` -> `ir_fuse_mem`, which checks the
address register (`ctx->regs[load_ref][2]`):

```c
if (reg != IR_REG_NONE) {
    return IR_MEM_B(reg);           // use allocated register as base
} else if (IR_IS_CONST_REF(addr)) {
    return ir_fuse_addr_const(...); // constant address
} else {
    return ir_fuse_addr(...);       // must be LEA -- asserts (see below)
}
```

Since the LOAD is fused and its constraints were never processed, `reg == IR_REG_NONE`.
The address is not a constant, so it falls through to `ir_fuse_addr`:

```c
IR_ASSERT(((rule & IR_RULE_MASK) >= IR_LEA_FIRST &&
           (rule & IR_RULE_MASK) <= IR_LEA_LAST) ||
          rule == IR_STATIC_ALLOCA);
switch (rule & IR_RULE_MASK) {
    default:
        IR_ASSERT(0);   // unreachable in theory
    case IR_LEA_OB:
        ...
}
```

### 3. In Release builds, `IR_ASSERT` is a no-op -- the emitter silently produces garbage

```c
#ifdef IR_DEBUG
# define IR_ASSERT(x) assert(x)
#else
# define IR_ASSERT(x)            // <-- no-op in Release
#endif
```

The assertion at `ir_fuse_addr` line 3712 fires (if Debug) but does nothing (if Release).
The switch `default:` also has `IR_ASSERT(0)` which is a no-op. Execution falls through to
the first case (`IR_LEA_OB`) which reads operands meant for a different instruction layout,
producing a garbage `ir_mem` result. The emitter generates x86 code using this garbage
memory operand.

Concretely, the O0 scratch register allocator gives the ADD only one GP register (rax).
The emitter:
1. Loads op1 (d_22 = 0x140) into rax
2. Needs to emit `add MEM, rax` where MEM is the fused load from d_4
3. To form the memory operand `[d_4]`, it must load d_4's spill slot into a register
4. The only register is rax -- **clobbers d_22**
5. Emits `add [rax], rax` which computes `d_4 + *(d_4)` instead of `*(d_4) + d_22`

## The Fix

**File:** `thirdparty/ir/ir_x86.dasc` -- functions `ir_match_fuse_load` and
`ir_match_try_fuse_load`

**Before (broken):** When the LOAD's address is a non-constant instruction,
unconditionally mark the load as fused and call `ir_match_fuse_addr`:

```c
} else {
    ctx->rules[ref] = IR_FUSED | IR_SIMPLE | IR_LOAD;   // always fuse
    ir_match_fuse_addr(ctx, addr_ref);                   // may silently fail
    return 1;
}
```

**After (fixed):** First ensure the address rule is something `ir_fuse_addr` can handle.
Only fuse the load if the address is LEA-compatible or `IR_STATIC_ALLOCA`:

```c
} else {
    uint32_t addr_rule = ctx->rules[addr_ref];
    if (!addr_rule) {
        addr_rule = ir_match_insn(ctx, addr_ref);
        ctx->rules[addr_ref] = addr_rule;
    }
    if (((addr_rule & IR_RULE_MASK) >= IR_LEA_FIRST &&
         (addr_rule & IR_RULE_MASK) <= IR_LEA_LAST) ||
        addr_rule == IR_STATIC_ALLOCA) {
        ctx->rules[ref] = IR_FUSED | IR_SIMPLE | IR_LOAD;
        ir_match_fuse_addr(ctx, addr_ref);
        return 1;
    }
    return 0;   // address not fusible -- keep LOAD as separate instruction
}
```

When the load is NOT fused, it gets emitted as a standalone `IR_LOAD_INT` instruction.
The constraint system allocates a register for its address via `IR_OP2_MUST_BE_IN_REG`,
and the result lands in a proper spill slot. The consuming ADD then reads both operands
from their spill slots (no fusion, no clobber).

### Why this only affects O0

| Opt level | `IR_OPT_CODEGEN` | ADD match result | Load fusion safe? |
|-----------|-------------------|------------------|-------------------|
| O0        | off               | `IR_BINOP_INT`   | no -- address not LEA, no register allocated |
| O1        | on                | `IR_LEA_*`       | yes -- address fused as LEA, or full RA resolves conflicts |
| O2        | on                | `IR_LEA_*`       | yes |

At O1/O2, `IR_OPT_CODEGEN` is set, so the ADD matcher produces LEA rules for address
calculations. `ir_match_fuse_addr` successfully fuses these, and the full register
allocator (`ir_reg_alloc`) handles any conflicts. The guard in the fix is trivially
true for O1/O2 -- no behavioral change.

### DynASM caveat

In `.dasc` files, `||` at the start of a line is a DynASM directive (action list "or").
The C logical-or operator `||` must never appear at column 1. The fix places `||` at
the end of the preceding line to avoid this:

```c
if (((addr_rule & IR_RULE_MASK) >= IR_LEA_FIRST &&     // && at end of line
     (addr_rule & IR_RULE_MASK) <= IR_LEA_LAST) ||      // || at end of line
    addr_rule == IR_STATIC_ALLOCA) {
```

## Test Results

All 27 sightglass kernels at all opt levels after the fix:

| Opt level | Pass | Fail | Notes |
|-----------|------|------|-------|
| O0        | 27   | 0    | was 0/27 before fix |
| O1        | 12   | 15   | pre-existing RA bug (`ir_ra.c:2441 ir_split_interval_at: Assertion p failed`) |
| O2        | 27   | 0    | no regression |
