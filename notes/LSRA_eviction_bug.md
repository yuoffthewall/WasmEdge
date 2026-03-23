# Bug: `def_reg != -1` assertion in `ir_emit_shift_const` (LSRA eviction drops interval)

## Symptoms

- **Assertion**: `def_reg != -1` at `ir_x86.dasc:5090` in `ir_emit_shift_const()`
- **Affected kernels**: `shootout-xblabla20`, `shootout-xchacha20` (both are cipher-heavy with high register pressure)
- **Affected opt levels**: O1 and O2 (debug builds). O0 is unaffected because it uses a different register allocator (`ir_allocate_unique_spill_slots()` during emit, not LSRA).
- **Crashing instruction**: `int64_t d_182 = ROL(d_181, c_31)` (IR_ROL, def ref=182, vreg=46). The `IR_SHIFT_CONST` emit rule requires `IR_DEF_REUSES_OP1_REG | IR_USE_MUST_BE_IN_REG`, but `ctx->regs[182][0]` is `IR_REG_NONE (-1)`.

## Root Cause

The bug is in `ir_allocate_blocked_reg()` in `thirdparty/ir/ir_ra.c`, in the code that evicts an active interval to free a register for a higher-priority interval.

### The eviction flow (lines ~3231-3282)

When the linear scan allocator decides to evict an active interval `other` to give its register to `ival`, it does:

**Step 1 — Primary split**: Try to split `other` before the conflict point so the prefix keeps the register:

```c
split_pos = ir_find_optimal_split_position(ctx, other, split_pos, ival->range.start, 1);
if (split_pos > other->range.start) {
    // CAN split: prefix keeps reg, child = suffix (no reg yet)
    child = ir_split_interval_at(ctx, other, split_pos);
    // remove other from active
} else {
    // CANNOT split: the entire interval loses its register
    child = other;
    other->reg = IR_REG_NONE;
    // remove other from active
}
```

**Step 2 — Secondary split**: Try to find the next use-in-reg position in `child` and split again so the tail can be re-queued for a new register:

```c
split_pos = ir_first_use_pos_after(child, ival->range.start,
    IR_USE_MUST_BE_IN_REG | IR_USE_SHOULD_BE_IN_REG) - 1;
if (split_pos > child->range.start && split_pos < child->end) {
    child2 = ir_split_interval_at(ctx, child, split_pos);
    ir_add_to_unhandled(unhandled, child2);  // re-queued ✓
} else if (child != other) {      // ← THE BUG
    ir_add_to_unhandled(unhandled, child);   // re-queued ✓
}
// implicit else: child == other → SILENTLY DROPPED ✗
```

### The bug: `else if (child != other)`

When the primary split fails (`child == other`, the interval fully loses its register) **AND** the secondary split also fails (the first use-in-reg is at the very start of `child`, so `split_pos <= child->range.start`), the final `else if (child != other)` evaluates to false. The interval is:

1. Removed from the active list
2. Has `reg = IR_REG_NONE`
3. **Not re-queued to unhandled**

This means `assign_regs()` later sees `ival->reg == IR_REG_NONE` and skips setting `ctx->regs[ref]`, which remains `IR_REG_NONE`. When the emitter processes the instruction and calls `ir_emit_shift_const()`, it reads `def_reg = IR_REG_NUM(ctx->regs[def][0])` and gets `-1`, triggering the assertion.

### Why only high register pressure triggers it

The bug requires a specific sequence:
1. An interval (vreg 46) gets a register (e.g., `reg=0`)
2. It is evicted by another interval that needs that same register
3. During eviction, the primary split position falls at or before the interval start (no room to split before the conflict)
4. The secondary split position also fails (first use-in-reg is at the interval start)
5. The `child != other` guard silently drops the interval

This only happens under high register pressure where intervals are evicted multiple times. The xblabla20/xchacha20 ciphers have 16 loop-carried PHI values competing for ~14 GP registers, creating exactly this scenario.

### Concrete trace for vreg 46 in shootout-xblabla20

```
1. Coalesced interval [310,845] gets reg=2 (RDX)
2. Evicted → split into prefix + child [341,845], child gets reg=0 (RAX) via re-queue
3. Child [341,845] is evicted AGAIN by another interval needing reg=0
4. Primary split fails: split_pos <= 341 (can't split before start)
   → child = other, other->reg = IR_REG_NONE
5. Secondary split: first_use_pos_after(child, ival->range.start, ...) - 1
   → split_pos <= child->range.start (use-in-reg is at the start)
   → Condition fails, falls to: else if (child != other) → FALSE
   → Interval DROPPED: no register, not re-queued
6. assign_regs() skips vreg 46 → ctx->regs[182][0] = IR_REG_NONE
7. ir_emit_shift_const() asserts def_reg != -1 → CRASH
```

## Fix

**File**: `thirdparty/ir/ir_ra.c`, lines 3275-3278 (after fix)

Changed the `else if (child != other)` to unconditional `else`:

```c
// BEFORE (buggy):
} else if (child != other) {
    ir_add_to_unhandled(unhandled, child);
    IR_LOG_LSRA("      ---- Queue", child, "");
}

// AFTER (fixed):
} else {
    ir_add_to_unhandled(unhandled, child);
    IR_LOG_LSRA("      ---- Queue", child, "");
}
```

This ensures that when an active interval is fully evicted (loses its register, can't be split), it is always re-queued to the unhandled list. The linear scan will then process it again and either:
- Allocate a different free register, or
- Evict something else, or
- Spill it to the stack

All of which are correct outcomes. The interval is never silently dropped.

### Why the original `child != other` guard existed

The guard was likely intended to prevent infinite loops: if `child == other` and we re-queue it with `reg = IR_REG_NONE`, the linear scan will process it again and might try to evict the same register, creating a cycle. However, this concern is unfounded because:
- When `child` is re-queued, `ival` now holds the register. So next time `child` is processed, the allocator sees different active/inactive sets and will make a different allocation decision.
- The existing `// TODO: this may cause endless loop` comment (which was already present on the `child != other` branch) acknowledges this theoretical concern but in practice the allocator converges because the register landscape changes between iterations.

## Verification

- `shootout-xblabla20` O1: **PASS** (was asserting)
- `shootout-xblabla20` O2: **PASS** (was asserting)
- `shootout-xchacha20` O1: **PASS** (was asserting)
- `shootout-xchacha20` O2: **PASS** (was asserting)
- Full sightglass suite O1: **all PASS**
- Full sightglass suite O2: **all PASS** (except `pulldown-cmark` which has a pre-existing stdout mismatch unrelated to this fix)
