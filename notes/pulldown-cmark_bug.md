# IR JIT O2 Miscompilation: pulldown-cmark output mismatch

## Status: FIXED

All 27 sightglass kernels pass at O2. Two pre-existing failures at
other opt levels (O0 shootout-base64, O1 regex) are separate bugs.

---

## Symptom

`pulldown-cmark` produced incorrect output at O2. O0 and O1 passed.
No crashes or assertions — silent wrong results.

## Root Causes

Two independent bugs, both O2-only, both required to fix pulldown-cmark.
Neither fix alone is sufficient — each independently corrupts the output.

- **Bug 1** corrupts **control flow**: wrong branches due to stale CPU flags
- **Bug 2** corrupts **data**: zero bytes written to memory where real values should be

Both bugs are gated by the SCCP pass (`ir_sccp`), which only runs at
`opt_level > 1`, explaining why O0 and O1 are unaffected.

---

## Bug 1: Stale-EFLAGS in `same_comparison` (ir_x86.dasc)

### The optimization

`ir_emit_cmp_and_branch_int` (`ir_x86.dasc:6750`) has a
`same_comparison` optimization: when two consecutive IFs compare the
same operands, the codegen skips the second CMP and reuses EFLAGS from
the first.

### The bug

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

### Why it only appeared at O2

`ir_iter_optimize_condition` (`ir_sccp.c:3390`) unwraps single-use ZEXT
from IF conditions:

```
IF(ZEXT(EQ(a,b)))  ->  IF(EQ(a,b))
```

This changes the match rule from `IR_IF_INT` (TEST+JNE) to
`IR_CMP_AND_BRANCH_INT` (CMP+JCC), making the `same_comparison` check
succeed where it previously didn't.

The unwrap only fires when MEM (load forwarding) cascades IF
instructions onto the iter_opt worklist — explaining the MEM+IF
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

## Bug 2: Promotion use-list corruption (ir_sccp.c)

### The optimization

During SCCP `ir_iter_opt`, TRUNC instructions are eliminated by
promoting their source tree to the narrower type. When the source is a
PHI, `ir_promote_i2i` (`ir_sccp.c:1799`) recurses into each PHI slot.
If a slot contains a ZEXT/SEXT/TRUNC whose source already matches the
target type, the extension node is stripped: its use-list entry for the
PHI is removed, and if no other users remain, the node is NOP'd.

The same logic exists in `ir_promote_d2f` (`ir_sccp.c:1598`) and
`ir_promote_f2d` (`ir_sccp.c:1658`) for floating-point promotions.

### The bug

`ir_use_list_remove_all` was used instead of `ir_use_list_remove_one`.
The promotion functions are called **once per PHI slot** via the PHI
loop (`ir_sccp.c:1903`):

```c
case IR_PHI:
    for (p = insn->ops + 2, n = insn->inputs_count - 1; n > 0; p++, n--) {
        input = *p;
        if (input != ref) {
            *p = ir_promote_i2i(ctx, type, input, ref, worklist);
        }
    }
```

When the same node (e.g., `ZEXT_38`) appears in N PHI slots:

1. **Slot 1**: `remove_all(ctx, ZEXT_38, PHI)` deletes ALL N use entries.
   Use count drops to 0. Node is NOP'd via `MAKE_NOP`. Returns `op1`
   (correct).
2. **Slots 2..N**: `ir_promote_i2i` is called with the same
   `ZEXT_38` ref. The node is now `IR_NOP` (line 1819). Returns a
   **zero constant** instead of the correct source operand.

### Concrete impact in pulldown-cmark

Two functions are affected (identified by instrumentation):

| Function (by insns_count) | Corrupted node | PHI ref | Slots affected | Type |
|---------------------------|---------------|---------|----------------|------|
| 2132 insns                | ZEXT ref=38 (4 uses) | PHI 1889 | 3 of 4 get zero | IR_U8 |
| 1176 insns                | ZEXT ref=34 (4 uses) | PHI 1159 | 3 of 4 get zero | IR_U8 |

Total: 6 corrupted PHI slots producing zero constants.

The corrupted zeros flow through: `PHI -> TRUNC -> STORE` — writing
zero bytes to memory where real U8 values (characters in the Markdown
parser) should be.

### Fix

Replaced `ir_use_list_remove_all` with `ir_use_list_remove_one` in all
three promotion functions (`ir_sccp.c`):

- `ir_promote_d2f` (line 1618)
- `ir_promote_f2d` (line 1681)
- `ir_promote_i2i` (line 1855)

This also removes the `count` variable and the compensating loops that
tried to re-add the excess removed entries (which were incorrect
anyway — they added uses pointing at the PHI for nodes that had already
been NOP'd):

```c
// Before (wrong):
count = ctx->use_lists[ref].count;
ir_use_list_remove_all(ctx, ref, use);
if (ctx->use_lists[ref].count == 0) {
    ir_use_list_replace_one(ctx, insn->op1, ref, use);
    if (count > 1) {
        do { ir_use_list_add(ctx, insn->op1, use); } while (--count > 1);
    }
    ...
}

// After (correct):
ir_use_list_remove_one(ctx, ref, use);
if (ctx->use_lists[ref].count == 0) {
    ir_use_list_replace_one(ctx, insn->op1, ref, use);
    ...
}
```

---

## Relationship Between the Two Bugs

The bugs are **independent** — they corrupt different things in
different pipeline stages:

| Aspect           | Bug 1 (same_comparison)       | Bug 2 (promotion use-list)     |
|------------------|-------------------------------|--------------------------------|
| Pipeline stage   | Codegen (x86 emission)        | SCCP optimization (ir_iter_opt)|
| What's corrupted | Control flow (wrong branches) | Data (zero constants in PHIs)  |
| Mechanism        | Stale CPU flags               | Premature NOP of shared nodes  |
| Downstream       | Wrong JCC target              | Zero bytes stored to memory    |

Both are O2-only because they depend on the SCCP pass:
- Bug 1 is enabled by ZEXT unwrap in `ir_iter_optimize_condition`,
  which changes the codegen match rule to `CMP_AND_BRANCH_INT`
- Bug 2 is directly in `ir_iter_opt`'s TRUNC promotion path

Verified experimentally:
- Fix 1 alone: FAILS (promotion corruption still writes zeros)
- Fix 2 alone: FAILS (stale EFLAGS still cause wrong branches)
- Both fixes: PASSES

---

## Commit

```
fix(ir): Fix O2 stale-EFLAGS in same_comparison and use-list corruption in promotions
```

Files changed:
- `ir_x86.dasc`: +6 -1 (adjacency guard)
- `ir_sccp.c`: +35 -43 (remove_all -> remove_one in 3 functions, delete compensating loops)
