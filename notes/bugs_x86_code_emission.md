# IR Backend Bugs: x86 Backend & Code Emission

Bugs in x86 code generation, DESSA moves, address fusion, and load fusion
(ir_x86.dasc, ir_emit.c, ir_cfg.c).

---

## Bug 1: Unsafe Load Fusion Produces Wrong x86 Code at O0

**Status: FIXED**

### Symptom

When running WasmEdge IR JIT benchmarks with `WASMEDGE_IR_JIT_OPT_LEVEL=0`, **all**
kernels that touch memory (everything except `noop`) segfault during execution.
Compilation succeeds; the crash happens in the generated JIT code at runtime.

### Example: shootout-ackermann

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

### Root Cause

The bug is a chain of three interacting design assumptions in the IR x86 backend
that break at O0.

#### 1. Load fusion marks the LOAD as consumed but doesn't allocate its address register

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

#### 2. At O0, address ADD is `IR_BINOP_INT` not LEA -- `ir_fuse_addr` can't handle it

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

#### 3. In Release builds, `IR_ASSERT` is a no-op -- the emitter silently produces garbage

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

### The Fix

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

#### Why This Only Affects O0

| Opt level | `IR_OPT_CODEGEN` | ADD match result | Load fusion safe? |
|-----------|-------------------|------------------|-------------------|
| O0        | off               | `IR_BINOP_INT`   | no -- address not LEA, no register allocated |
| O1        | on                | `IR_LEA_*`       | yes -- address fused as LEA, or full RA resolves conflicts |
| O2        | on                | `IR_LEA_*`       | yes |

At O1/O2, `IR_OPT_CODEGEN` is set, so the ADD matcher produces LEA rules for address
calculations. `ir_match_fuse_addr` successfully fuses these, and the full register
allocator (`ir_reg_alloc`) handles any conflicts. The guard in the fix is trivially
true for O1/O2 -- no behavioral change.

#### DynASM Caveat

In `.dasc` files, `||` at the start of a line is a DynASM directive (action list "or").
The C logical-or operator `||` must never appear at column 1. The fix places `||` at
the end of the preceding line to avoid this:

```c
if (((addr_rule & IR_RULE_MASK) >= IR_LEA_FIRST &&     // && at end of line
     (addr_rule & IR_RULE_MASK) <= IR_LEA_LAST) ||      // || at end of line
    addr_rule == IR_STATIC_ALLOCA) {
```

### Test Results

All 27 sightglass kernels at all opt levels after the fix:

| Opt level | Pass | Fail | Notes |
|-----------|------|------|-------|
| O0        | 27   | 0    | was 0/27 before fix |
| O1        | 12   | 15   | pre-existing RA bug (`ir_ra.c:2441 ir_split_interval_at: Assertion p failed`) |
| O2        | 27   | 0    | no regression |

---

## Bug 2: Stale EFLAGS in `same_comparison` (ir_x86.dasc)

**Status: FIXED**

### The Optimization

`ir_emit_cmp_and_branch_int` (`ir_x86.dasc:6750`) has a
`same_comparison` optimization: when two consecutive IFs compare the
same operands, the codegen skips the second CMP and reuses EFLAGS from
the first.

### The Bug

The old guard only checked that the IF's control predecessor was
`IF_TRUE` or `IF_FALSE` (no LOAD/STORE in the control chain). But data
instructions (ADD, SUB, AND, OR, XOR, etc.) are NOT in the control
chain and DO clobber EFLAGS:

```
Block A:
  CMP a, b          <- first IF (CMP_AND_BRANCH_INT)
  JL  block_X

Block B (IF_TRUE from Block A):
  ... data insns ... <- ADD, SUB, AND, etc. clobber EFLAGS
  [CMP a, b]        <- SKIPPED by same_comparison
  JGE block_Y       <- uses stale flags -> WRONG BRANCH
```

### Why It Only Appeared at O2

`ir_iter_optimize_condition` (`ir_sccp.c:3390`) unwraps single-use ZEXT
from IF conditions:

```
IF(ZEXT(EQ(a,b)))  ->  IF(EQ(a,b))
```

This changes the match rule from `IR_IF_INT` (TEST+JNE) to
`IR_CMP_AND_BRANCH_INT` (CMP+JCC), making the `same_comparison` check
succeed where it previously didn't.

The unwrap only fires when MEM (load forwarding) cascades IF
instructions onto the iter_opt worklist -- explaining the MEM+IF
interaction.

### Scale

~70 unsafe flag-reuse sites across pulldown-cmark functions (detected
by instrumentation checking `insn->op1 != def - 1` with matching
comparison operands).

### Fix

Added adjacency check `insn->op1 == def - 1` so flags are only reused
when the IF is immediately after the block start with zero intervening
instructions (`ir_x86.dasc:6752`):

```c
// Before (unsafe):
if (prev_insn->op == IR_IF_TRUE || prev_insn->op == IR_IF_FALSE) {

// After (safe):
if ((prev_insn->op == IR_IF_TRUE || prev_insn->op == IR_IF_FALSE)
 && insn->op1 == def - 1) {
```

---

## Bug 3: `stk_tmp` Clobbers Register Arg at O0

**Status: FIXED**

### Symptom

At O0, four Sightglass kernels failed: `blake3-scalar`, `gcc-loops`,
`pulldown-cmark`, `shootout-ackermann`. `shootout-ackermann` output `M = 0 and
N = 0` instead of `M = 3 and N = 7`.

### Root Cause

**File:** `thirdparty/ir/ir_x86.dasc`, function `ir_emit_arguments()`, pass 3.

Pass 3 stack-argument copies originally used `tmp_reg` as scratch. The CALL
instruction's constraint system (`ir_get_target_constraints`) allocates a
temporary register at slot 1 for indirect calls. `ir_emit_call` passes
`ctx->regs[def][1]` as `tmp_reg` to `ir_emit_arguments`. At O0, the simple
allocator assigned **RCX** to this slot.

The problem: pass 3 processes register args first (loading from spill slots into
RDI, RSI, RDX, **RCX**, R8, R9), then processes stack args using `tmp_reg`
(= RCX) for load-then-store. The stack-arg setup **clobbered the 4th register
argument** (RCX) that was already loaded:

```asm
;; Pass 3 register arg setup:
mov    0x2c(%rsp),  %ecx        ; arg3 = p1 (correct value in RCX)
mov    0x170(%rsp), %r8d        ; arg4 = d_94
mov    0x190(%rsp), %r9         ; arg5 = d_99

;; Pass 3 stack arg setup -- CLOBBERS ECX:
mov    0x188(%rsp), %rcx        ; tmp_reg=RCX: loading d_98 for stack
mov    %rcx,        (%rsp)      ; -> stack arg 0  (ECX now = d_98, not p1!)
...
call   *0x1b0(%rsp)             ; ECX = wrong value
```

### Fix

Introduced a dedicated `stk_tmp = IR_REG_RAX` for stack-argument copies instead
of reusing `tmp_reg`. RAX is never a parameter register on SysV or Windows
x86-64, so it cannot conflict with register arguments loaded earlier in pass 3.

```c
// Before (broken):
// used tmp_reg directly -- could be any scratch reg (RCX at O0)

// After (fixed):
ir_reg stk_tmp = IR_REG_RAX;
ir_emit_load(ctx, type, stk_tmp, arg);
ir_emit_store_mem_int(ctx, type, mem, stk_tmp);
```

The original `tmp_reg` is still passed to `ir_parallel_copy` in pass 2, where
it is used as a swap register for resolving register copy cycles. This avoids
any regression at O1/O2.

---

## Bug 4: x86 Address-Fusion Assertion with ir_MUL_A Pattern

**Status: WORKAROUND (upstream fix needed)**

### Syndrome
`ir_x86.dasc:1871: ir_match_fuse_addr_all_useges: Assertion '((insn->type) < IR_DOUBLE) && ir_type_size[insn->type] >= 4' failed`
during `ir_jit_compile` at O2.

### Reproducer (before the workaround)
```
WASMEDGE_SIGHTGLASS_KERNEL=pulldown-cmark WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

### Root Cause
The x86 backend's address-fusion pass (`ir_match_fuse_addr_all_useges`)
tries to fold address arithmetic into x86 addressing modes
(base + index*scale + disp). When the IR contains
`ir_MUL_A(CONST_ADDR(funcIdx), CONST_ADDR(8))` fed into a `LOAD_A`, the
fusion pass walks the operand chain and encounters a `TRUNC` to `uint8_t`
(IR type size 1) in a surrounding instruction. The assertion requires
`ir_type_size[insn->type] >= 4`, which fails for 1-byte types.

The `TRUNC` to `uint8_t` exists in the baseline IR too (from Wasm
`i32.store8` patterns), but the old call pattern (5-arg `CALL` through a
single reused pointer) didn't create the `MUL_A` + `ADD_A` chain that
causes the fusion pass to walk into those instructions.

### Workaround (in ir_builder.cpp)
Pre-compute the FuncTable byte offset as a single constant instead of
using `ir_MUL_A`:
```cpp
// Old: ir_MUL_A(ir_CONST_ADDR(funcIdx), ir_CONST_ADDR(sizeof(void*)))
// New: ir_CONST_ADDR(funcIdx * sizeof(void*))
ir_ref FuncPtr = ir_LOAD_A(ir_ADD_A(
    ValidFT,
    ir_CONST_ADDR(static_cast<uintptr_t>(ResolvedFuncIdx) * sizeof(void *))));
```
Since `funcIdx` is always a compile-time constant, this is both correct
and simpler.

### Proper Fix (upstream)
`ir_match_fuse_addr_all_useges` should either skip instructions with
sub-4-byte types when walking address chains, or not assert on them.

---

## Bug 5: x86 Address-Fusion Assertion with IR_VOID Indirect Call Return

**Status: WORKAROUND (upstream fix needed)**

### Syndrome
Same assertion as Bug 4:
`ir_x86.dasc:1871: ir_match_fuse_addr_all_useges: Assertion '((insn->type) < IR_DOUBLE) && ir_type_size[insn->type] >= 4' failed`

### Reproducer (before the workaround)
Same as Bug 4 (pulldown-cmark at O2), but only triggered when a
void-returning function is called through an indirect `CALL` with
`IR_VOID` return type in the prototype.

### Root Cause
When a `CALL` instruction has `IR_VOID` return type and is followed by
other address-computation instructions, the O2 optimizer's address-fusion
pass encounters the void-typed CALL node during its operand walk.
`IR_VOID` has `ir_type_size = 0`, failing the `>= 4` check.

The old code always returned `IR_I64` from `jit_direct_or_host` (even
for void callees), so this never arose.

### Workaround (in ir_builder.cpp)
Always declare `IR_I64` return type in the call prototype, even for
void-returning callees. The unused `rax` value is harmless on x86-64:
```cpp
ir_type DirectRetType;
if (RetType == IR_FLOAT) {
    DirectRetType = IR_FLOAT;
} else if (RetType == IR_DOUBLE) {
    DirectRetType = IR_DOUBLE;
} else {
    DirectRetType = IR_I64;  // includes void -- avoids IR_VOID assertion
}
```

### Proper Fix (upstream)
Same as Bug 4 -- the address-fusion pass should handle or skip
`IR_VOID`-typed nodes gracefully.

---

## Bug 6: DESSA Parallel Copy Duplicate Destinations

**Status: FIXED**

### Symptom

```
ir_emit.c:748: ir_dessa_parallel_copy: Assertion `!ir_bitset_in(todo, to)' failed.
```

Affected kernels at O1: shootout-fib2, quicksort, and many others.

### Example: shootout-fib2 (function 016)

The Wasm frontend (`ir_builder.cpp`) emits one PHI per Wasm local at each merge
point. When two locals hold the same value, the IR contains identical PHI nodes:

```
; at MERGE l_165 -- two PHIs with identical inputs
d_169 = PHI(l_165, c_12, d_125)
d_172 = PHI(l_165, c_12, d_125)
```

During `ir_coalesce()`, both d_169 and d_172 share input d_125. The coalescer
merges them into the same virtual register (say vreg 90) via `ir_vregs_coalesce`.

Later, `ir_emit_dessa_moves()` iterates over all PHIs at the successor MERGE.
For each PHI whose output is vreg V and whose input on this edge is X, it
generates a copy `from=X, to=V`. Since both d_169 and d_172 map to vreg 90,
the function produces:

```
copy[1]: from=-12, to=90    (from c_12 to vreg 90)
copy[4]: from=-12, to=90    (from c_12 to vreg 90, duplicate)
```

In some cases the coalesced PHIs have different inputs on a given edge (because
different locals converge from different definitions) but all map to the same
output vreg. This produces many copies with the same `to` but different `from`:

```
copy[0]:  from=1848 to=1   (rcx)
copy[1]:  from=392  to=1   (rcx)
copy[2]:  from=1849 to=1   (rcx)
...
copy[24]: from=546  to=1   (rcx)
```

`ir_dessa_parallel_copy()` builds a `pred[to] = from` map and a `todo` bitset.
On the second copy targeting the same `to`, the assertion fires:

```c
// ir_emit.c line 738-749
for (i = 0; i < count; i++) {
    from = copies[i].from;
    to = copies[i].to;
    ...
    pred[to] = from;          // overwrites silently
    types[to] = copies[i].type;
    IR_ASSERT(!ir_bitset_in(todo, to));  // FIRES on duplicate `to`
    ir_bitset_incl(todo, to);
}
```

### Root Cause

`ir_emit_dessa_moves()` (`ir_emit.c:834`) iterates every PHI at a successor
MERGE and unconditionally adds a copy for each one. It has no awareness that
coalescing may have merged multiple PHIs to the same vreg, making their copies
redundant.

When multiple PHIs are coalesced to vreg V, the coalescing invariant guarantees
that on any given edge, all their inputs hold the same value at runtime. Therefore
all copies `from=X_i, to=V` are semantically equivalent -- executing any single
one is sufficient.

### Fix

**File:** `thirdparty/ir/ir_emit.c` -- function `ir_emit_dessa_moves`

Before adding a copy to the `copies[]` array, scan existing entries for one with
the same `to` destination. If found, skip the duplicate:

```c
if (to != from) {
    int j, dup = 0;
    // (existing spill-slot dedup check unchanged)
    ...
    /* Skip duplicate copies to the same destination that arise when
       multiple PHI nodes are coalesced to the same virtual register.
       All such copies carry the same value (coalescing invariant),
       so keeping any one of them is sufficient. */
    for (j = 0; j < (int)n; j++) {
        if (copies[j].to == to) {
            dup = 1;
            break;
        }
    }
    if (dup) continue;
    copies[n].type = insn->type;
    copies[n].from = from;
    copies[n].to = to;
    n++;
}
```

The linear scan is O(n^2) in the number of PHIs per MERGE, but n is typically
small (< 50) and this runs once per block edge, so the cost is negligible.

---

## Bug 7: Trivial PHI Chains Causing Wrong Branch Decisions + Dead PHI DESSA Clobbering

**Status: FIXED**

Two independent bugs at O1, both caused by the WasmEdge frontend emitting
one PHI per Wasm local at every merge point.

### Bug 7a: Trivial PHI Chains (regex)

#### Symptom

`regex` crashes at O1 with an `unreachable` trap in Wasm function 755. The trap
cascades through `jit_host_call` (which returns 0 for the error), function 788
then also traps, and eventually the process SEGVs. At O0 the execution takes a
code path that avoids the trap. At O2 the trap path is eliminated by SCCP
constant folding. At O1 (without SCCP), the function takes the WRONG code path
entirely.

#### Root Cause

The WasmEdge frontend emits one PHI per Wasm local at every loop header and
merge point, regardless of whether the local was modified on any incoming edge.
When all incoming edges carry the same value, the PHI is **trivial** -- it's
semantically a no-op copy. When loops are nested, trivial PHIs form chains
through successive loop headers, each reading the previous trivial PHI.

##### What a Trivial PHI Is

In SSA form, a PHI node merges values from different control-flow predecessors.
A trivial PHI is one where all non-self-referencing inputs are the same value:

```
d_243 = PHI(l_242, d_21, d_21)
```

Both arms of the merge provide `d_21`, so `d_243 == d_21` always. It contributes
nothing semantically -- it's a copy that the compiler doesn't recognize as one.

##### How the WasmEdge Frontend Creates Them

The WasmEdge-to-IR builder (`lib/vm/ir_builder.cpp`) emits one PHI per Wasm
local at every loop header via `visitLoop()`. If a Wasm local holds value `d_21`
on all incoming edges (i.e., it was never modified inside the loop or on any
branch reaching this merge), the PHI still gets created with identical inputs on
every edge. The frontend doesn't check whether the local was actually modified --
it unconditionally emits a PHI for every local at every merge point.

When loops are nested, this compounds. Each nesting level adds another trivial
PHI that reads the previous one:

**Concrete example from regex func 99 (dump_id 086) at O1 without SCCP:**

```
d_21       = LOAD(...)                              -- original value (a Wasm local)
d_243      = PHI(l_242, d_21, d_21)                 -- loop header level 1, trivial
d_5408     = PHI/3(l_5407, d_243, d_243, d_21)      -- loop header level 2, trivial
d_5501     = PHI/3(l_5500, d_21, d_21, d_5408)      -- loop header level 3, trivial
d_5622     = PHI(l_5621, d_21, d_5501)              -- merge, non-trivial (d_21 vs d_5501)
```

Every node from `d_243` through `d_5501` is semantically identical to `d_21`,
but the compiler treats each as a distinct SSA value. `d_5622` LOOKS non-trivial
(inputs `d_21` vs `d_5501`), but since `d_5501 == d_21`, it's also trivially
`d_21`.

##### Scale of the Problem in func 99 (dump_id 086)

| Metric | O1 (no SCCP) | O1 + SCCP |
|--------|-------------|-----------|
| Total PHIs | 670 | 149 |
| Trivial PHIs (all inputs identical) | 98 (14.6%) | 1 (0.7%) |
| Total IR lines | 6288 | 4871 |
| Total DESSA MOVs at codegen | 769 | -- |
| DESSA MOVs for trivial PHIs | 194 (25.7%) | -- |

SCCP eliminates 521 PHIs (78%), including nearly all 98 trivial ones. The 194
wasted DESSA MOVs (25.7% of all DESSA MOVs) are pure overhead that also create
opportunities for register clobbering.

##### How Trivial PHIs Break Register Allocation

The register allocator treats each PHI as a distinct SSA value with its own
virtual register, regardless of whether it's trivial. This creates a cascade:

1. **Different vregs for the same value.** `ir_assign_virtual_registers()` gives
   each node its own vreg. Some get coalesced back, others don't.

2. **DESSA moves between vregs at every merge point.** At each block boundary
   where a PHI lives, the DESSA pass emits parallel copy instructions.

3. **Spill slot pressure and copy errors.** When the RA spills trivial PHIs to
   different stack slots, DESSA moves become memory-to-memory copies routed
   through a scratch register. With many MOVs at a single merge point, the
   emitter must carefully sequence them to avoid clobbering.

4. **Wrong branch decisions.** In func 99 of the regex benchmark, a trivial PHI
   chain carries a value used in a branch condition. A DESSA move clobbers the
   register holding the value, causing the wrong path to execute.

##### Why O2 Doesn't Have This Bug

At O2, SCCP replaces all uses of trivial PHIs with the original value directly.
670 PHIs -> 149 PHIs. No unnecessary copies, no clobbering, correct branch.

##### Why O0 Doesn't Have This Bug

At O0, there is no register allocation at all. Trivial PHIs still exist but
don't cause harm without RA.

#### Fix

**File:** `thirdparty/ir/ir.h` -- function `ir_jit_compile`

Enable SCCP at O1 (was previously only enabled at O2):

```c
// BEFORE:
if (opt_level > 1) {
    if (!ir_sccp(ctx)) { return NULL; }
}

// AFTER:
if (opt_level > 0) {
    if (!ir_sccp(ctx)) { return NULL; }
}
```

SCCP is lightweight (single-pass lattice solver) and eliminates all trivial PHI
chains by propagating constants and copy values.

#### Approaches That Were Tried and Failed

1. **Turning trivial PHIs into IR_NOP.** Broke compilation -- IR backend requires
   PHIs at merge points for CFG structural integrity.

2. **Replacing trivial PHI inputs with the original value.** Allocator still
   assigned different vregs because they were still distinct SSA values.

3. **Full trivial PHI elimination (replace all uses).** Caused "double free or
   corruption" heap crashes -- use-list manipulation corrupted IR data structures.

4. **Custom trivial-PHI elimination pass in ir.h.** Still crashed with heap
   corruption when modifying use lists.

### Bug 7b: Dead PHI DESSA Moves Clobbering Live Registers (rust-protobuf)

#### Symptom

`rust-protobuf` SEGVs in `wasm_jit_016` at O1. GDB shows the faulting
instruction `mov 0x0(%rbp,%r12,1),%r10d` using r12=GlobalBase (exec_env[16])
where it should use MemoryBase (exec_env[24]). The register was silently
overwritten between function entry and the crash point.

#### Root Cause

The WasmEdge frontend emits one PHI per Wasm local at every merge point. Many
of these are **dead PHIs** -- their data output is never used by any instruction.
In function 016 of rust-protobuf: 110 out of 267 PHIs are dead.

At O2, SCCP eliminates dead PHIs before register allocation. At O0, there is
no register allocation. But at O1, dead PHIs survive into the RA pipeline:

1. `ir_assign_virtual_registers()` assigns vregs to ALL data nodes including
   dead PHIs
2. `ir_compute_live_ranges()` creates minimal intervals for dead PHIs
3. `ir_reg_alloc()` assigns physical registers to these intervals. Because
   the dead PHI's range is tiny, the allocator may assign it the same physical
   register as a live value whose range doesn't overlap at that exact point.
4. `ir_emit_dessa_moves()` generates parallel copy moves for dead PHIs just
   like live ones.
5. These dead DESSA copies write to registers that are supposed to hold live
   values, silently clobbering them.

#### Fix

**File:** `thirdparty/ir/ir_emit.c` -- function `ir_emit_dessa_moves`

Skip PHIs with zero data uses (i.e., `use_lists[ref].count == 0`):

```c
if (insn->op == IR_PHI) {
    ...
    /* Skip dead PHIs (no data uses). Their DESSA moves are
       unnecessary and can clobber registers holding live values
       because the RA assigns dead PHIs minimal live ranges
       that may overlap with other intervals. */
    if (ctx->use_lists[ref].count == 0) {
        continue;
    }
    ...
}
```

#### Relationship to Bug 7a

With the SCCP fix (Bug 7a), most dead PHIs are already eliminated before
register allocation. The dead-PHI DESSA skip is therefore a **defense-in-depth**
fix: it protects against any dead PHIs that survive SCCP.

### Test Results After All Fixes

| Level | Pass | Fail | Notes |
|-------|------|------|-------|
| O0    | 27   | 0    | no regression |
| O1    | 25   | 2    | was 21/27; regex, rust-protobuf, rust-json now pass |
| O2    | 25   | 2    | no regression |

Remaining failures (pre-existing):
- `shootout-xblabla20`: assertion `def_reg != -1` in `ir_emit_shift_const`
- `shootout-xchacha20`: same assertion as xblabla20
- `pulldown-cmark`: produces wrong output at O1

### Verified Non-Issues from Previous Notes

The previous `current_bug.md` hypothesized several frontend bugs that were
investigated and found to be **incorrect**:

1. **"ValueStack Corruption in Control Flow"** -- No evidence of stack pointer
   mismanagement in the IR builder. The symptom was actually caused by dead PHI
   register clobbering.

2. **"Circular Loop PHI Self-References"** -- Searched all IR dumps for
   self-referencing PHIs. Found zero instances. The WasmEdge frontend does NOT
   generate self-referencing PHIs.

### Debug Tips

These techniques proved useful during the investigation of these bugs.

#### 1. Dump IR before and after compilation

Set `WASMEDGE_IR_JIT_DUMP=1` to dump IR for every compiled function to
`/tmp/wasmedge_ir_NNN_before.ir` and `/tmp/wasmedge_ir_NNN_after.ir`.
The dump ID maps to Wasm function index as: `func_index = dump_id + ImportFuncNum`.

#### 2. Add codegen dump to ir.h

Add an `ir_dump_codegen` call in `ir_jit_compile()` right before `ir_emit_code`
to see register assignments and DESSA MOVs.

#### 3. Identify trivial PHIs with a script

```bash
python3 -c "
import re
with open('/tmp/wasmedge_ir_086_after.ir') as f:
    for line in f:
        m = re.search(r'(d_\d+)\s.*= PHI\S*\((\S+),\s*(.+)\)', line)
        if m:
            inputs = [x.strip() for x in m.group(3).split(',')]
            if len(set(inputs)) == 1:
                print(f'TRIVIAL: {m.group(1)} = PHI({m.group(2)}, {m.group(3)})')
"
```

#### 4. GDB with JIT symbols

```gdb
break wasm_jit_086
run
disas
```

#### 5. JitExecEnv layout reference

```
Offset  Field           Type        Register (at entry)
[0]     FuncTable       void**      loaded early
[8]     FuncTableSize   uint32_t
[12]    _pad            uint32_t
[16]    GlobalBase      uint8_t*    often r12 (callee-saved)
[24]    MemoryBase      uint8_t*    often r13 (callee-saved)
[32]    HostCallFn      void*       jit_host_call pointer
[40]    DirectOrHostFn  void*       jit_direct_or_host pointer
[48]    MemoryGrowFn    void*
[56]    MemorySizeFn    void*
[64]    CallIndirectFn  void*
```

---

## Bug 8: Empty-Block Cycle in ir_emit Causes Infinite Loop at O0

**Status: FIXED**

### Syndrome
The `shootout-base64` sightglass kernel hangs indefinitely during JIT
**compilation** (never reaches execution) at O0. The hang is in
`_ir_skip_empty_blocks()` (`ir_cfg.c:1296`), which loops forever through
a cycle of two empty blocks. O1 and O2 are unaffected because optimizer
passes (SCCP, DCE) transform or eliminate the pattern before code
emission.

### Reproducer
```
WASMEDGE_SIGHTGLASS_KERNEL=shootout-base64 WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=0 \
./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

### Root Cause
The Wasm module contains an empty infinite loop -- the IR equivalent of
`(loop $L (br $L))` -- which the WasmEdge IR builder correctly translates
to:
```
LOOP_BEGIN(entry_edge, back_edge)   // block 9
END                                  // block 9
BEGIN                                // block 10
LOOP_END                             // block 10  -> successor = block 9
```

During code emission (`ir_emit.c`, the instruction-matching phase), each
block is checked for emptiness: if a block contains only a start and end
instruction (no computation), and its sole successor is not itself, it is
marked `IR_BB_EMPTY`. The self-loop guard
(`ctx->cfg_edges[bb->successors] != b`) only catches the trivial case of
a block whose successor is itself. It does **not** detect multi-block
cycles: block 9 points to block 10 (9 != 10, passes the check), and
block 10 points back to block 9 (10 != 9, also passes). Both blocks are
marked `IR_BB_EMPTY`.

Later, when emitting an `IF` instruction whose true branch leads through
a chain of empty blocks into this cycle, `ir_get_true_false_blocks()`
calls `ir_skip_empty_target_blocks()` -> `_ir_skip_empty_blocks()`, which
follows the successor chain: block 3 (empty) -> block 8 (empty) -> block 9
(empty) -> block 10 (empty) -> block 9 -> ... -- infinite loop.

### Fix (applied to thirdparty/ir)

1. **`ir_emit.c` (primary):** Before marking a block as `IR_BB_EMPTY`,
   follow its successor chain through already-marked-empty blocks. If
   the chain leads back to the current block, skip the `IR_BB_EMPTY`
   marking. The un-marked block retains its label in the emission phase
   and emits its `END`/`LOOP_END` as a jump, correctly generating the
   empty infinite loop in machine code (a `jmp` back to itself through
   the empty chain).

2. **`ir_cfg.c` (safety fallback):** Added an iteration counter to
   `_ir_skip_empty_blocks()` bounded by `cfg_blocks_count`. If
   exceeded, asserts (debug builds) and returns the current block to
   prevent hangs from any unforeseen empty-block cycle.

---

## Bug 9: DESSA Parallel Copy Spill-Slot Aliasing (Lost Copy)

**Status: FIXED**

### Symptom

The `rust-compression` sightglass kernel hits an `unreachable` trap (0x40a) at IR
JIT O1. The crash is non-deterministic in *which* function traps, but deterministic
in *which* function is miscompiled: compile index 63. The miscompiled function
produces wrong values written to wasm linear memory, causing later functions to take
wrong branches into trap stubs.

O0 passes, O2 passes (SCCP changes IR enough to avoid the LSRA aliasing), O1 fails.

### Bisection

```
WASMEDGE_IR_JIT_BISECT=63 → passes (func 63 compiled at O2)
WASMEDGE_IR_JIT_BISECT=64 → fails  (func 63 compiled at O1)
```

At BISECT=64, func 63 is called once and makes 1830 calls to func 64 before
crashing — the outer loop counter d_80 is stuck at 1, never incrementing. Expected
call count is 55.

### Root Cause

The LSRA assigns two different vregs to the same spill slot:

```
d_78 = PHI(0, d_80)       R30, SPILL=0xcc
d_80 = PHI(1, d_156)      R31, SPILL=0xd0
d_156 = d_80 + 1           R58, SPILL=0xcc  ← SAME SLOT AS d_78
```

On BB4's loop back-edge, `ir_dessa_parallel_copy()` must emit the parallel
assignment `{d_78 ← d_80, d_80 ← d_156}` as sequential moves. The algorithm
tracks dependencies by **vreg ID** (R30, R31, R58), not by physical spill slot.
It doesn't detect that writing R30 (d_78) to slot 0xcc clobbers R58 (d_156) at the
same slot.

The emitted asm:

```asm
mov 0xd0(%rsp), %edx    ; edx = d_80
mov %edx, 0xcc(%rsp)    ; d_78 ← d_80  (OVERWRITES d_156 at 0xcc!)
mov 0xcc(%rsp), %edx    ; edx = d_80    (was supposed to be d_156)
mov %edx, 0xd0(%rsp)    ; d_80 ← d_80  (NO-OP — BUG)
```

Result: d_80 = PHI(1, d_156) always reads back d_80's own value. The loop variable
never advances past 1. After ~183 "iterations" (1830 inner calls), the memory
pointer walks past valid wasm memory, producing garbage values that cascade into the
trap.

### Fix

Added a canonicalization pass in `ir_emit_dessa_moves()` (`ir_emit.c`), inserted
after building the copy list and before calling `ir_dessa_parallel_copy()`.

For every pair of copies, if copy[i].to is a spill-slot vreg and copy[j].from is a
*different* spill-slot vreg at the **same physical slot**, replace copy[j].from with
copy[i].to. This makes the physical aliasing visible to the parallel copy algorithm's
dependency tracking, which then either reorders the moves or uses a temporary register
to break the cycle.

After canonicalization, any copies that became no-ops (from == to) are removed.

The fix is O(n²) in the number of PHI copies per edge, which is small in practice
(typically 2–15 copies).

### Why O2 passes

SCCP at O2 constant-folds branches and eliminates ~33 blocks. The simplified IR
changes live ranges, and the LSRA no longer assigns d_78 and d_156 to the same
spill slot.

### Key evidence

1. **Return-address tracing:** All 1830 F64 calls return to offset 0xcb7 (BB4 loop
   body). The alternative code path (BB21+ loop) is never executed.
2. **Per-call arg logging:** First outer iteration (d_80=1) produces correct 10
   inner calls. All subsequent iterations have d_80=1 — the counter never advances.
3. **RA debug dump:** R30 (d_78) SPILL=0xcc, R58 (d_156) SPILL=0xcc — confirmed
   same slot.
4. **Native code verification:** Disassembly at 0xb91–0xba6 shows the lost copy
   sequence described above.
