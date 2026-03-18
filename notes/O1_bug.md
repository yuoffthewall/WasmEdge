# IR JIT O1 Bugs: DESSA Duplicate Copies and Dead PHI Inverted Live Ranges

## Overview

Two independent bugs prevented most sightglass kernels from compiling at
`WASMEDGE_IR_JIT_OPT_LEVEL=1` (and, with assertions enabled, some at O2 as well).
Both stem from the WasmEdge frontend generating duplicate PHI nodes — one per Wasm
local — that the IR backend was not prepared to handle after coalescing.

After the fixes, O1 goes from 12/27 to 21/27 passing kernels in debug builds
(24/27 in release). The remaining failures are pre-existing, unrelated bugs.

---

## Bug 1: DESSA Parallel Copy Duplicate Destinations

### Symptom

```
ir_emit.c:748: ir_dessa_parallel_copy: Assertion `!ir_bitset_in(todo, to)' failed.
```

Affected kernels at O1: shootout-fib2, quicksort, and many others.

### Example: shootout-fib2 (function 016)

The Wasm frontend (`ir_builder.cpp`) emits one PHI per Wasm local at each merge
point. When two locals hold the same value, the IR contains identical PHI nodes:

```
; at MERGE l_165 — two PHIs with identical inputs
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
all copies `from=X_i, to=V` are semantically equivalent — executing any single
one is sufficient.

### Fix

**File:** `thirdparty/ir/ir_emit.c` — function `ir_emit_dessa_moves`

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

The linear scan is O(n²) in the number of PHIs per MERGE, but n is typically
small (< 50) and this runs once per block edge, so the cost is negligible.

---

## Bug 2: Dead PHI Inverted Live Range

### Symptom

```
ir_ra.c:2441: ir_split_interval_at: Assertion `p' failed.
```

Affected kernel at O1: shootout-ackermann (function 018, vreg 236).

### Background: Live Position Encoding

Each IR ref `r` maps to 4 sub-positions in the live range timeline:

```
r*4+0  LOAD_SUB_REF   — operand loads
r*4+1  USE_SUB_REF    — operand uses
r*4+2  DEF_SUB_REF    — result definition
r*4+3  SAVE_SUB_REF   — result spill/save
```

A valid live range `[start, end)` requires `start < end`.

### The Bug

In `ir_compute_live_ranges()` (`ir_ra.c`), when a PHI node has no uses (dead
PHI), the code creates a minimal live interval:

```c
// ir_ra.c line 800-803 (and identically at line 1430-1433)
ival = ctx->live_intervals[v];
if (UNEXPECTED(!ival)) {
    /* Dead PHI */
    ival = ir_add_live_range(ctx, v,
        IR_DEF_LIVE_POS_FROM_REF(ref),    // = ref*4 + 2
        IR_USE_LIVE_POS_FROM_REF(ref));   // = ref*4 + 1  ← LESS THAN START
}
```

This creates an **inverted range** `[ref*4+2, ref*4+1)` where `start > end`.

For example, with ref=653: range `[2614, 2613)` — start 2614 > end 2613.

During coalescing, a dead PHI's interval may be joined with a live interval
(they share an input). Later, the register allocator calls `ir_split_interval_at`
on the combined interval at a position that falls within the dead PHI's
degenerate range:

```c
// ir_ra.c line 2435-2441
p = &ival->range;
prev = NULL;
while (p && pos >= p->end) {   // for [3310,3309): 3310 >= 3309 is TRUE
    prev = p;
    p = prev->next;            // walks past the degenerate range
}
IR_ASSERT(p);                  // p is NULL → assertion fires
```

The loop condition `pos >= p->end` is always true for inverted ranges (since
`end < start <= pos`), so the loop walks past them. If the degenerate range
is the last in the chain, `p` becomes NULL.

### Why This Only Affects O1

At O2, SCCP eliminates dead PHIs before register allocation. At O0, there is
no register allocation at all (uses virtual registers with spills). At O1, SCCP
is disabled but full register allocation runs, so dead PHIs survive into the RA
where their inverted ranges cause the assertion.

### Fix

**File:** `thirdparty/ir/ir_ra.c` — two sites in `ir_compute_live_ranges()`

Replace `IR_USE_LIVE_POS_FROM_REF(ref)` with `IR_SAVE_LIVE_POS_FROM_REF(ref)`
to create a proper minimal range `[DEF, SAVE)` = `[ref*4+2, ref*4+3)`:

```c
// ir_ra.c line 802 and line 1432
ival = ir_add_live_range(ctx, v,
    IR_DEF_LIVE_POS_FROM_REF(ref),     // = ref*4 + 2
    IR_SAVE_LIVE_POS_FROM_REF(ref));   // = ref*4 + 3  ← CORRECT: end > start
```

This gives the dead PHI a 1-unit live range at its definition point. The range
is valid (non-inverted, non-zero-width), so `ir_split_interval_at` can find it
if it ever needs to split there.

The two sites correspond to the two copies of `ir_compute_live_ranges` in the
file — one for the general case and one for the SCC-optimized variant.

---

## Test Results

All 27 sightglass kernels at all opt levels after both fixes:

### O0

| Pass | Fail | Notes |
|------|------|-------|
| 27   | 0    | no regression from O0 fix |

### O1 (debug build, assertions enabled)

| Pass | Fail | Notes |
|------|------|-------|
| 21   | 6    | was ~12/27 before fix |

Remaining O1 failures:
- `pulldown-cmark`: `ir_fuse_addr: Assertion 'base_reg != -1'` (pre-existing RA bug, also fires at O2 debug)
- `shootout-xblabla20`, `shootout-xchacha20`: `ir_emit_shift_const: Assertion 'def_reg != -1'` (pre-existing RA bug, also fires at O2 debug)
- `regex`: runtime `unreachable` trap (wrong code generation, passes at O2)
- `rust-json`, `rust-protobuf`: timeout (likely infinite loop in JIT code)

### O1 (release build, assertions disabled)

| Pass | Fail | Notes |
|------|------|-------|
| 24   | 3    | xblabla20/xchacha20 pass since assertion is no-op |

### O2

| Pass | Fail | Notes |
|------|------|-------|
| 27   | 0    | no regression (release); 24/27 debug (pre-existing) |
