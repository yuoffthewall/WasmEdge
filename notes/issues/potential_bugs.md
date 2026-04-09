# Potential Register Allocation Bugs in `thirdparty/ir/ir_ra.c`

Analysis of the Linear Scan Register Allocator (LSRA) implementation based on
Wimmer & Franz, CGO'10. Focus on bugs that cause **register clobbering** (two
live values assigned the same physical register) and other correctness issues.

---

## Part 1 — Register Clobbering Bugs

These bugs can cause two simultaneously-live values to occupy the same physical
register, producing silent miscompilation.

### Clobbering Bug 1: `current_range` Becomes Stale After Splitting Inactive Intervals

**Location:** `ir_allocate_blocked_reg` lines 3287–3308, interacting with LSRA
main loop lines 3557–3593

**Severity:** Critical

When `ir_allocate_blocked_reg` splits a conflicting inactive interval:

```c
// ir_ra.c:3287-3305
other = *inactive;
while (other) {
    if (reg == other->reg) {
        ir_live_pos overlap = ir_ivals_overlap(&ival->range, other->current_range);
        if (overlap) {
            child = ir_split_interval_at(ctx, other, overlap);
            other->current_range = &other->range;  // reset cache
            ir_add_to_unhandled(unhandled, child);
        }
    }
    other = other->list_next;  // continues iterating inactive list
}
```

`ir_split_interval_at` mutates `other`'s ranges — truncating them to end at
`overlap`. But `other` stays on the inactive list with its old register
assignment (`other->reg` unchanged). Only `child` is re-queued.

Then at line 3311, `ival` is assigned the same register `reg`. Now both `other`
(still on inactive with reg) and `ival` (now on active with reg) hold the same
physical register.

When the LSRA main loop later processes an inactive→active transition
(lines 3580–3588):

```c
if (position >= r->start) {
    /* move i from inactive to active */
    other->list_next = active;
    active = other;
}
```

If `other`'s truncated range is still alive at a later position, both `ival`
and `other` become active simultaneously on the same register → **clobber**.

---

### Clobbering Bug 2: Overlap Check Uses `current_range` — Misses Earlier Sub-Ranges

**Location:** `ir_try_allocate_free_reg` line 2876

**Severity:** Critical

```c
next = ir_ivals_overlap(&ival->range, other->current_range);
```

For inactive intervals, the overlap check starts at `other->current_range`,
which is a cached pointer that advances forward during the LSRA loop
(lines 3561–3578). If `current_range` has advanced past an earlier sub-range
(e.g., one added by splitting), the overlap check **misses** conflicts:

```
other's full ranges:  [10..20) [30..40) [50..60)
other->current_range: --------^
                      (points to [30..40), range [10..20) is skipped)

ival's range:         [15..25)
ir_ivals_overlap(ival->range, other->current_range) starts at [30..40)
→ returns 0 (no overlap!)
→ But ival [15..25) DOES overlap with other [10..20)
```

The `freeUntilPos` for this register stays at `0x7fffffff` → the register
appears completely free → **clobber**.

This happens when:
1. An inactive interval gets split, producing a child with new ranges
2. The parent interval stays on inactive with `current_range` advanced
3. A new interval overlaps with the parent's earlier (already-passed) ranges

---

### Clobbering Bug 3: Active Eviction Breaks After First Match — Misses Scratch Sets

**Location:** `ir_allocate_blocked_reg` lines 3225–3285

**Severity:** High

```c
prev = NULL;
other = *active;
while (other) {
    if (reg == other->reg) {
        // split and evict this interval
        // ...
        break;  // <--- LINE 3281: BREAKS AFTER FIRST MATCH
    }
    prev = other;
    other = other->list_next;
}
```

After evicting one conflicting active interval, the loop `break`s. For regular
registers, only one interval can be active per register, so this is fine. But
for scratch pseudo-registers (`reg >= IR_REG_NUM`), the scratch regset maps to
multiple physical registers. The comparison `reg == other->reg` compares the
chosen *physical* register against `other->reg` which might be a
*pseudo-register* — they'll never be equal, so fixed/scratch intervals sharing
the same physical register are **never evicted**.

In `ir_try_allocate_free_reg`, scratch intervals cause the entire scratch
regset to be removed from `available` (line 2858). So LSRA normally can't pick
a register in an active scratch set. The clobbering path requires the scratch
interval to be **inactive**, where its `freeUntilPos` is only reduced at the
overlap point (subject to `current_range` caching from Bug 2). If the overlap
check misses, the scratch's physical registers appear free → clobber.

Bug 3 amplifies Bug 2 for scratch/call-clobber registers.

---

### Clobbering Bug 4: Fused Spill-Load Skipped When Register Is Actually Needed

**Location:** `assign_regs` lines 3911–3929

**Severity:** Medium-High

```c
if (!(use_pos->flags & IR_USE_MUST_BE_IN_REG)
 && use_pos->hint != reg
 && ctx->ir_base[ref].op != IR_SNAPSHOT
 && !needs_spill_load(ctx, ival, use_pos)) {
    /* fuse spill load (valid only when register is not reused) */
    reg = IR_REG_NONE;
    // ...
}
```

The condition `use_pos->hint != reg` is supposed to prevent fusing when the
register will be reused. But `use_pos->hint` is a *preferred* register (hint),
not the *actually allocated* register for subsequent uses. If the hint was never
set (`IR_REG_NONE`) but the register IS reused by a later instruction, the
condition passes and the spill load is skipped. The next instruction reads the
register expecting the spilled value but gets whatever was last written there →
**clobber / stale value**.

`needs_spill_load` (line 3780) partially guards against this by checking
whether there are more uses, but it only checks `use_pos->next->op_num != 0` —
it doesn't verify whether the next use requires the same or a different
register.

---

### Clobbering Bug 5: Recycled Range Node Aliased by Another Interval's `current_range`

**Location:** `ir_split_interval_at` lines 2491–2496

**Severity:** Medium

```c
if (pos == p->start) {
    prev->next = NULL;
    ival->end = prev->end;
    p->next = ctx->unused_ranges;  // recycle p into the free pool
    ctx->unused_ranges = p;
}
```

The range node `p` is recycled into `unused_ranges`. If another interval's
`current_range` happens to point to this same node (possible through the
prepend-swap trick in `ir_add_live_range`, lines 198–211, which copies the
embedded first range to a new node), then `current_range` now points to a node
that will be reused for a completely different interval. Subsequent overlap
calculations produce garbage → wrong allocation → **clobber**.

The `unused_ranges` pool is a global recycling mechanism — any
`ir_add_live_range` or `ir_add_fixed_live_range` call can pick up a recycled
node and overwrite its `start`/`end`/`next` fields.

---

### Clobbering Bug 6: Coalescing Doesn't Recheck Against Fixed Intervals

**Location:** `ir_coalesce` lines 1945–1979

**Severity:** Medium-High

```c
// ir_ra.c:1966-1978
r->end = IR_LOAD_LIVE_POS_FROM_REF(input);  // shrink range temporarily
if (ir_vregs_overlap(ctx, v1, v2)) {
    // restore range
} else {
    ir_swap_operands(ctx, input, input_insn);
    IR_ASSERT(!ir_vregs_overlap(ctx, v1, v2));  // only checks v1 vs v2!
    ir_vregs_coalesce(ctx, v1, v2, input, use);
}
```

`ir_vregs_overlap` only checks v1 against v2. After coalescing, the merged
interval may overlap with a **fixed register interval** that neither v1 nor v2
overlapped with individually. The coalescing phase doesn't re-check against
fixed intervals. If the merged range spans across a CALL clobber or a
fixed-register constraint, the LSRA may not fully detect this (especially when
combined with `current_range` caching issues from Bugs 1–2).

---

## Part 2 — General Correctness Bugs

### Bug 7: NULL Dereference in `ir_split_interval_at` When `pos == p->start`

**Location:** `ir_ra.c:2491–2493`

**Severity:** Medium-High

```c
if (pos == p->start) {
    prev->next = NULL;       // prev can be NULL here!
    ival->end = prev->end;
```

When `pos` falls exactly at a later range's start and the while loop at
line 2437 (`while (p && pos >= p->end)`) advances `prev` past all earlier
ranges, `prev` will be non-NULL. But if the interval has only one range and
`pos == p->start`, then `p == &ival->range` and `prev == NULL`.

The assertion `IR_ASSERT(pos > ival->range.start)` at line 2432 prevents the
single-range case. However, after coalescing and range merging, edge cases with
very short or zero-length ranges can bypass the assertion guard.

---

### Bug 8: Infinite Loop in `ir_allocate_blocked_reg`

**Location:** `ir_ra.c:3265–3278`

**Severity:** High

```c
split_pos = ir_first_use_pos_after(child, ival->range.start,
    IR_USE_MUST_BE_IN_REG | IR_USE_SHOULD_BE_IN_REG) - 1;
if (split_pos > child->range.start && split_pos < child->end) {
    // ... split and queue child2
} else {
    // TODO: this may cause endless loop       <-- author's own comment
    ir_add_to_unhandled(unhandled, child);
}
```

When `child` can't be split (split position at or before `child->range.start`),
it is re-queued to `unhandled` unchanged. The LSRA main loop picks it up,
fails to allocate, and re-queues it again — indefinitely.

---

### Bug 9: `ir_first_use_pos_after` Return Value Underflow

**Location:** `ir_ra.c:3199–3207, 3265`

**Severity:** Medium

`ir_first_use_pos_after` returns `0x7fffffff` when no use position is found.
Subtracting 1 gives `0x7ffffffe` — a position far beyond any real interval.
`ir_split_interval_at` then tries to split at this absurd position, potentially
producing a degenerate child interval or hitting assertions.

At line 3203:
```c
split_pos = ir_first_use_pos_after(ival, blockPos[reg],
    IR_USE_MUST_BE_IN_REG | IR_USE_SHOULD_BE_IN_REG) - 1;
other = ir_split_interval_at(ctx, ival, split_pos);
```

And at line 3265:
```c
split_pos = ir_first_use_pos_after(child, ival->range.start,
    IR_USE_MUST_BE_IN_REG | IR_USE_SHOULD_BE_IN_REG) - 1;
```

Both sites blindly subtract 1 without checking the sentinel return.

---

### Bug 10: `ir_try_allocate_preferred_reg` / `ir_get_preferred_reg` NULL Dereference

**Location:** `ir_ra.c:2688–2689, 2714–2715`

**Severity:** Medium

```c
if (use_pos->hint_ref > 0) {
    reg = ctx->live_intervals[ctx->vregs[use_pos->hint_ref]]->reg;
```

No NULL check on `ctx->vregs[use_pos->hint_ref]` (could be 0 if the ref has
no vreg) or on the resulting `ctx->live_intervals[...]` (could be NULL after
coalescing consumed and NULLed one side). Either case dereferences NULL.

---

### Bug 11: Stale `ival->end` Cache

**Location:** Throughout — `ival->end` is used as a cached copy of the last
range's end (`ir_private.h:1277`)

**Severity:** Medium

Multiple code paths temporarily mutate ranges (e.g., `ir_try_swap_operands`
at line 1851 shrinks `r->end`, checks overlap, then restores). If any code
path forgets to restore `ival->end` after a temporary mutation, or if a merge
in `ir_add_live_range` produces a shorter tail (shouldn't happen but possible
after other mutations), the cache becomes stale. Since `ival->end` is used
for early-exit comparisons throughout the LSRA, a stale value can cause
missed overlaps → wrong register assignment.

---

### Bug 12: `select_register` Goto Loop With Stale `nextUsePos`/`blockPos`

**Location:** `ir_allocate_blocked_reg` lines 3155–3217

**Severity:** Medium

```c
select_register:
    reg = IR_REGSET_FIRST(available);
    // ... find reg with highest nextUsePos ...
    if (split_pos >= blockPos[reg]) {
        IR_REGSET_EXCL(available, reg);
        if (IR_REGSET_IS_EMPTY(available)) { exit(-1); }
        goto select_register;
    }
```

Each iteration removes one register and retries, but `nextUsePos[]` and
`blockPos[]` are **not** recomputed. The algorithm picks the "best" remaining
register from stale data. The new best register's `blockPos` might also be too
early, causing repeated exclusions until all registers are exhausted, hitting
the fatal error exit.

---

### Bug 13: `ir_add_live_range` Prepend-Swap Invalidates External Pointers

**Location:** `ir_ra.c:198–211`

**Severity:** Medium-High

```c
// New range entirely before first range — swap trick
q->start = p->start;   // p is &ival->range (embedded)
q->end = p->end;
q->next = p->next;
p->start = start;       // overwrite embedded range with new data
p->end = end;
p->next = q;
```

Since the first range is embedded in the `ir_live_interval` struct, it can't
be prepended normally. Instead, the old first range data is copied to a new
node `q`, and the embedded slot is overwritten with the new range.

Any existing pointer to `&ival->range` (such as `other->current_range` in the
LSRA loop, set at line 3511 or 3540) now sees different `start`/`end` values.
If the LSRA is in the middle of processing active/inactive transitions when
this happens (e.g., during `ir_allocate_blocked_reg` which calls
`ir_split_interval_at` which calls `ir_add_live_range`), the stale pointer
causes the LSRA to make wrong active/inactive/handled decisions → missed
interference → potential clobber.

---

### Bug 14: Use-Position Merge Ordering Asymmetry During Coalescing

**Location:** `ir_vregs_join` line 1623

**Severity:** Low-Medium

```c
while (*prev && ((*prev)->pos < use_pos->pos ||
    ((*prev)->pos == use_pos->pos &&
        (use_pos->op_num == 0 || ((*prev)->op_num != 0 && (*prev)->op_num < use_pos->op_num))))) {
```

The secondary sort when positions are equal: definitions (`op_num == 0`) are
always inserted before uses at the same position, but the relative order of
two definitions at the same position is non-deterministic (depends on which
list they came from). Two definitions at the same position after coalescing
is unusual but possible with PHI nodes and can lead to non-deterministic
register hint resolution.

---

## Summary Table

| # | Bug | Type | Severity | Symptom |
|---|-----|------|----------|---------|
| 1 | Stale `current_range` after splitting inactive | Clobber | Critical | Two intervals active on same reg |
| 2 | Overlap check misses earlier sub-ranges via `current_range` | Clobber | Critical | Register appears free when occupied |
| 3 | Active eviction breaks after first match (scratch sets) | Clobber | High | Scratch interval unevicted |
| 4 | Fused spill-load skipped, stale register read | Clobber | Medium-High | Wrong value in register |
| 5 | Recycled range node aliased by `current_range` | Clobber | Medium | Garbage overlap calculation |
| 6 | Coalescing skips recheck vs fixed intervals | Clobber | Medium-High | Merged interval spans fixed constraint |
| 7 | NULL deref in `ir_split_interval_at` | Crash | Medium-High | SEGV during RA |
| 8 | Infinite loop in `ir_allocate_blocked_reg` | Hang | High | Compilation hangs |
| 9 | `ir_first_use_pos_after` underflow | Miscompile | Medium | Bad split position |
| 10 | NULL deref in hint resolution | Crash | Medium | SEGV during LSRA |
| 11 | Stale `ival->end` cache | Clobber | Medium | Missed overlap |
| 12 | `select_register` goto with stale data | Fatal error | Medium | "Allocation not possible" abort |
| 13 | Prepend-swap invalidates `current_range` pointers | Clobber | Medium-High | Wrong active/inactive decisions |
| 14 | Use-position merge ordering asymmetry | Non-determinism | Low-Medium | Inconsistent hint resolution |

**Most likely root causes of register clobbering:** Bugs 1, 2, and 13 — all
stem from the `current_range` caching mechanism. The LSRA advances
`current_range` forward for performance, but splitting, range mutation, and the
embedded-range swap trick can leave the cache pointing at stale or wrong data,
creating blind spots in interference detection.
