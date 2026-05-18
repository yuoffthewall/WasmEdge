# IR Backend Bugs: Register Allocation (LSRA)

Bugs in the dstogov/ir library's register allocator -- LSRA, live ranges,
coalescing, spill/reload (ir_ra.c).

---

## Bug 1: ir_compute_live_ranges Crash from Stale Use-Lists

**Status: FIXED**

### Symptom
IR library register allocator crashes during compilation of complex functions.
regex (1/907 funcs) and rust-json (1/442 funcs) segfault during
instantiate() -- at compile time, not runtime.

Crash location: ir_ra.c, inside ir_compute_live_ranges(), in the linked-list
code path (the default -- IR_BITSET_LIVENESS is commented out in ir_private.h).

### Root Cause -- Detailed Explanation

The crash is a NULL dereference of ctx->live_intervals[v]. This array is
indexed by virtual register number. A live interval is supposed to be created
for every vreg that is live (has defs and uses). The crash means a vreg exists
(has an assigned number) but was never given a live interval.

#### How Live Intervals Are Created (linked-list path)

1. ir_compute_live_sets() runs first. It walks all vregs, finds their uses
   (including PHI uses in successor blocks), and populates live_outs[b] -- a
   linked list of vregs that are live at the exit of block b.

2. The main loop (line 1281) iterates blocks in reverse order. For each block:
   a. For each vreg in live_outs[b], it calls ir_add_prev_live_range() which
      CREATES the live interval if it doesn't exist (line 1290).
   b. For PHI inputs in successor blocks, it reads ctx->live_intervals[v]
      directly (line 1316) -- NO creation, just access.
   c. For each instruction def in reverse order, it calls ir_fix_live_range()
      which also assumes the interval already exists (line 1411).

So the only place intervals are created is step 2a, via live_outs. If a vreg
is missing from live_outs, its interval is never created, and steps 2b/2c crash.

Compare with the bitset path (line 694): it calls ir_add_prev_live_range() for
PHI inputs too, which creates on demand. The linked-list path is less defensive.

#### Why Vregs Go Missing from live_outs

ir_compute_live_sets() finds PHI uses by iterating the use_list of each vreg.
If a vreg's use_list is stale (doesn't include a PHI that actually references
it), the vreg won't be added to the predecessor block's live_outs.

Use-lists become stale through this sequence:

1. Our IR builder creates PHI nodes for ALL locals at loop entry, even
   unmodified ones. Since ir_COPY is folded by ir_fold.h (line 1579: COPY
   returns op1 directly), Locals[idx] for an unmodified local still equals
   the PHI ref. At back-edge, this produces self-referencing PHIs:
   d_X = PHI(loop, init, d_X).

2. SCCP correctly identifies these: ir_sccp_analyze_phi (ir_sccp.c:370)
   skips self-references (`if (input == i) continue`), so PHI(loop, init,
   d_X) evaluates to COPY(init). ir_sccp_replace_insn replaces the PHI.

3. ir_iter_opt runs after SCCP (ir_sccp.c:3777). It performs dead code
   removal, merge optimization, and loop optimization. These passes modify
   instructions and incrementally update use-lists. But when SCCP replaced
   a PHI and ir_iter_opt further transforms the graph, some use-list entries
   can become inconsistent -- a PHI still references a value, but the value's
   use_list no longer includes that PHI.

4. ir_compute_live_sets iterates the stale use_list, doesn't find the PHI,
   doesn't add the vreg to live_outs. No live interval is created.

#### The Three Crash Sites in ir_compute_live_ranges

All three assume ctx->live_intervals[v] is non-NULL:

1. Line 1316 (PHI input in predecessor block):
   ival = ctx->live_intervals[v];
   ir_add_phi_use(ctx, ival, ...);  // NULL deref if missing

2. Line 776/1406 (IR_PARAM flag setting):
   if (insn->op == IR_PARAM) {
       ctx->live_intervals[v]->flags |= ...;  // NULL deref if missing
   }

3. Line 785/1411 (ir_fix_live_range for instruction defs):
   ival = ir_fix_live_range(ctx, v, ...);
   // ir_fix_live_range accesses ctx->live_intervals[v]->range internally

#### Why Reducing PHIs Alone Does NOT Fix the Bug

We tried two approaches to reduce trivial PHIs:

**Fix: Skip unmodified locals at back-edge (emitLoopBackEdge)**
Changed self-referencing PHI(loop, init, d_X) to trivial PHI(loop, init, init).
This fixed the line 1316 crash because SCCP folds PHI(loop, init, init) more
cleanly than self-referencing PHIs. But line 1411 still crashed.

The reason: the stale use-list issue is not limited to PHI inputs. When SCCP
folds a trivial PHI to its input value and ir_iter_opt removes the dead PHI,
other instructions that were defined in the same block may also have their
use-lists become inconsistent. Specifically, a non-PHI instruction's def can
have a vreg that ir_compute_live_sets misses because the use_list chain was
broken during dead code removal. This vreg never appears in any block's
live_outs, so ir_fix_live_range at line 1411 dereferences NULL.

In other words: reducing PHIs reduces the TRIGGER (fewer stale use-list entries)
but doesn't eliminate the VULNERABILITY (the linked-list path's assumption that
every vreg has a live interval). The stale use-list problem is inherent to how
SCCP + ir_iter_opt update use-lists during transformation.

#### Why NULL Guards Are Necessary
The linked-list path in ir_compute_live_ranges has a structural gap: it assumes
ir_compute_live_sets populated live_outs correctly for every reachable vreg.
The bitset path (line 694) doesn't make this assumption -- it creates intervals
on demand via ir_add_prev_live_range. The NULL guards align the linked-list
path's behavior with the bitset path, making it resilient to stale use-lists
regardless of what optimization passes do upstream.

### Fix -- Both Sides

IR builder (ir_builder.cpp):
- emitLoopBackEdge: skip unmodified locals (Locals[idx] == PhiRef check)
- visitEnd: wire unmodified PHIs to pre-loop value, restore Locals[idx] to
  pre-loop value so the PHI becomes dead (no users). SCCP eliminates it.
  This produces cleaner IR and reduces compilation work, but is NOT sufficient
  to prevent the RA crash on its own.

IR library (ir_ra.c):
- Line 776/1406: guard IR_PARAM flag with NULL check
- Line 785/1411: if live_intervals[v] is NULL, call ir_add_live_range to
  create a minimal range instead of ir_fix_live_range which would deref NULL
- Line 1316: if live_intervals[v] is NULL, call ir_add_prev_live_range to
  create a range spanning the block (matching bitset path at line 694)

SIGSEGV guard (ir_jit_engine.cpp):
- Retained as defense-in-depth for unknown future IR library bugs.

Additional fixes:
- Replaced ir_ZEXT_U32() calls with coerceToType() for trampoline arguments.
- ir_sccp.c: handle NOP'd instructions in ir_promote_i2i (return zero constant).
- ir_sccp.c: disable ir_try_promote_induction_var_ext (breaks wasm 32-bit
  wrapping semantics -- see bugs_sccp_optimization.md Bug 1).

Result: All sightglass benchmarks compile with zero failures.

---

## Bug 2: LSRA Eviction Drops Interval (`def_reg != -1`)

**Status: FIXED**

### Symptoms

- **Assertion**: `def_reg != -1` at `ir_x86.dasc:5090` in `ir_emit_shift_const()`
- **Affected kernels**: `shootout-xblabla20`, `shootout-xchacha20` (both are cipher-heavy with high register pressure)
- **Affected opt levels**: O1 and O2 (debug builds). O0 is unaffected because it uses a different register allocator (`ir_allocate_unique_spill_slots()` during emit, not LSRA).
- **Crashing instruction**: `int64_t d_182 = ROL(d_181, c_31)` (IR_ROL, def ref=182, vreg=46). The `IR_SHIFT_CONST` emit rule requires `IR_DEF_REUSES_OP1_REG | IR_USE_MUST_BE_IN_REG`, but `ctx->regs[182][0]` is `IR_REG_NONE (-1)`.

### Root Cause

The bug is in `ir_allocate_blocked_reg()` in `thirdparty/ir/ir_ra.c`, in the code that evicts an active interval to free a register for a higher-priority interval.

#### The Eviction Flow (lines ~3231-3282)

When the linear scan allocator decides to evict an active interval `other` to give its register to `ival`, it does:

**Step 1 -- Primary split**: Try to split `other` before the conflict point so the prefix keeps the register:

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

**Step 2 -- Secondary split**: Try to find the next use-in-reg position in `child` and split again so the tail can be re-queued for a new register:

```c
split_pos = ir_first_use_pos_after(child, ival->range.start,
    IR_USE_MUST_BE_IN_REG | IR_USE_SHOULD_BE_IN_REG) - 1;
if (split_pos > child->range.start && split_pos < child->end) {
    child2 = ir_split_interval_at(ctx, child, split_pos);
    ir_add_to_unhandled(unhandled, child2);  // re-queued
} else if (child != other) {      // <-- THE BUG
    ir_add_to_unhandled(unhandled, child);   // re-queued
}
// implicit else: child == other -> SILENTLY DROPPED
```

#### The Bug: `else if (child != other)`

When the primary split fails (`child == other`, the interval fully loses its register) **AND** the secondary split also fails (the first use-in-reg is at the very start of `child`, so `split_pos <= child->range.start`), the final `else if (child != other)` evaluates to false. The interval is:

1. Removed from the active list
2. Has `reg = IR_REG_NONE`
3. **Not re-queued to unhandled**

This means `assign_regs()` later sees `ival->reg == IR_REG_NONE` and skips setting `ctx->regs[ref]`, which remains `IR_REG_NONE`. When the emitter processes the instruction and calls `ir_emit_shift_const()`, it reads `def_reg = IR_REG_NUM(ctx->regs[def][0])` and gets `-1`, triggering the assertion.

#### Why Only High Register Pressure Triggers It

The bug requires a specific sequence:
1. An interval (vreg 46) gets a register (e.g., `reg=0`)
2. It is evicted by another interval that needs that same register
3. During eviction, the primary split position falls at or before the interval start (no room to split before the conflict)
4. The secondary split position also fails (first use-in-reg is at the interval start)
5. The `child != other` guard silently drops the interval

This only happens under high register pressure where intervals are evicted multiple times. The xblabla20/xchacha20 ciphers have 16 loop-carried PHI values competing for ~14 GP registers, creating exactly this scenario.

#### Concrete Trace for vreg 46 in shootout-xblabla20

```
1. Coalesced interval [310,845] gets reg=2 (RDX)
2. Evicted -> split into prefix + child [341,845], child gets reg=0 (RAX) via re-queue
3. Child [341,845] is evicted AGAIN by another interval needing reg=0
4. Primary split fails: split_pos <= 341 (can't split before start)
   -> child = other, other->reg = IR_REG_NONE
5. Secondary split: first_use_pos_after(child, ival->range.start, ...) - 1
   -> split_pos <= child->range.start (use-in-reg is at the start)
   -> Condition fails, falls to: else if (child != other) -> FALSE
   -> Interval DROPPED: no register, not re-queued
6. assign_regs() skips vreg 46 -> ctx->regs[182][0] = IR_REG_NONE
7. ir_emit_shift_const() asserts def_reg != -1 -> CRASH
```

### Fix

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

#### Why the Original `child != other` Guard Existed

The guard was likely intended to prevent infinite loops: if `child == other` and we re-queue it with `reg = IR_REG_NONE`, the linear scan will process it again and might try to evict the same register, creating a cycle. However, this concern is unfounded because:
- When `child` is re-queued, `ival` now holds the register. So next time `child` is processed, the allocator sees different active/inactive sets and will make a different allocation decision.
- The existing `// TODO: this may cause endless loop` comment (which was already present on the `child != other` branch) acknowledges this theoretical concern but in practice the allocator converges because the register landscape changes between iterations.

### Verification

- `shootout-xblabla20` O1: **PASS** (was asserting)
- `shootout-xblabla20` O2: **PASS** (was asserting)
- `shootout-xchacha20` O1: **PASS** (was asserting)
- `shootout-xchacha20` O2: **PASS** (was asserting)
- Full sightglass suite O1: **all PASS**
- Full sightglass suite O2: **all PASS** (except `pulldown-cmark` which has a pre-existing stdout mismatch unrelated to this fix)

---

## Bug 3: Dead PHI Inverted Live Range

**Status: FIXED**

### Symptom

```
ir_ra.c:2441: ir_split_interval_at: Assertion `p' failed.
```

Affected kernel at O1: shootout-ackermann (function 018, vreg 236).

### Background: Live Position Encoding

Each IR ref `r` maps to 4 sub-positions in the live range timeline:

```
r*4+0  LOAD_SUB_REF   -- operand loads
r*4+1  USE_SUB_REF    -- operand uses
r*4+2  DEF_SUB_REF    -- result definition
r*4+3  SAVE_SUB_REF   -- result spill/save
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
        IR_USE_LIVE_POS_FROM_REF(ref));   // = ref*4 + 1  <-- LESS THAN START
}
```

This creates an **inverted range** `[ref*4+2, ref*4+1)` where `start > end`.

For example, with ref=653: range `[2614, 2613)` -- start 2614 > end 2613.

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
IR_ASSERT(p);                  // p is NULL -> assertion fires
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

**File:** `thirdparty/ir/ir_ra.c` -- two sites in `ir_compute_live_ranges()`

Replace `IR_USE_LIVE_POS_FROM_REF(ref)` with `IR_SAVE_LIVE_POS_FROM_REF(ref)`
to create a proper minimal range `[DEF, SAVE)` = `[ref*4+2, ref*4+3)`:

```c
// ir_ra.c line 802 and line 1432
ival = ir_add_live_range(ctx, v,
    IR_DEF_LIVE_POS_FROM_REF(ref),     // = ref*4 + 2
    IR_SAVE_LIVE_POS_FROM_REF(ref));   // = ref*4 + 3  <-- CORRECT: end > start
```

This gives the dead PHI a 1-unit live range at its definition point. The range
is valid (non-inverted, non-zero-width), so `ir_split_interval_at` can find it
if it ever needs to split there.

The two sites correspond to the two copies of `ir_compute_live_ranges` in the
file -- one for the general case and one for the SCC-optimized variant.

---

## Bug 4: prev_use_ref Skips Spill-Reload for Same-Instruction Duplicate Args

**Status: FIXED**

### Symptom

`shootout-ed25519` crashes at O2 in `wasm_jit_003` with a SIGSEGV on an
out-of-bounds memory access. The caller (`wasm_jit_002`) passes garbage in a
parameter register.

### Root Cause

**File:** `thirdparty/ir/ir_ra.c`, function `assign_regs()` (line ~3896).

The LSRA walks each live interval's use positions and decides whether the value
needs a spill-reload (`IR_REG_SPILL_LOAD`) at each use site. As an
optimization, it tracks `prev_use_ref` -- the last instruction that used this
interval -- and skips the `needs_spill_reload()` check when the current use is
in the same basic block as the previous use (since `needs_spill_reload` works at
block granularity, not instruction granularity).

The bug: when the **same variable appears twice as different operands of the same
CALL instruction**, both uses share the same `ref`. On the first use,
`needs_spill_reload()` correctly returns true (a prior CALL in the block
clobbered the caller-saved register), and the allocator sets
`reg |= IR_REG_SPILL_LOAD`, then sets `prev_use_ref = ref`.

On the **second** use at the same CALL, `prev_use_ref == ref`, so
`ctx->cfg_map[prev_use_ref] == ctx->cfg_map[ref]`, the entire condition
short-circuits to **false**. The allocator falls through to the "reuse register
without spill load" path, assigning a **plain register** (no SPILL_LOAD flag).

### Fix

**File:** `thirdparty/ir/ir_ra.c`, line 3896.

Added `prev_use_ref == ref` to force re-evaluation of `needs_spill_reload` when
two uses of the same variable are at the **same instruction**:

```c
// Before (broken):
if ((!prev_use_ref || ctx->cfg_map[prev_use_ref] != ctx->cfg_map[ref])
 && needs_spill_reload(ctx, ival, ctx->cfg_map[ref], available)) {

// After (fixed):
if ((!prev_use_ref || prev_use_ref == ref || ctx->cfg_map[prev_use_ref] != ctx->cfg_map[ref])
 && needs_spill_reload(ctx, ival, ctx->cfg_map[ref], available)) {
```

---

## Bug 5: Dead Load Skips Spill Reload, Stale Register on Alternate CFG Path

**Status: FIXED**

### Symptom

At O1, three Sightglass kernels crashed with SIGSEGV: `blind-sig`,
`pulldown-cmark`, `regex`. A callee-saved register (`rbp`) holding a wasm
memory base pointer contained a stale function-parameter value instead of the
correct spilled value, causing invalid memory accesses. All three passed at O0
and O2.

### How to Reproduce

```shell
cd build
WASMEDGE_SIGHTGLASS_MODE=IR_JIT WASMEDGE_SIGHTGLASS_SKIP_INTERP=1 \
  WASMEDGE_IR_JIT_OPT_LEVEL=1 WASMEDGE_SIGHTGLASS_KERNEL=blind-sig \
  ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

Standalone reproduction with the `ir` tool:

```shell
cd thirdparty/ir
./ir -O1 -S /tmp/blind_sig_082_O1_before.ir   # produces asm with missing reload
```

### Root Cause

**File:** `thirdparty/ir/ir_ra.c`, functions `assign_regs()` and
`ir_emit_load_int()` in `ir_emit_x86.h`.

The linear scan register allocator's `assign_regs()` function tracks which
basic blocks have an "available" copy of a spilled register (i.e., the register
was reloaded from the spill slot in that block). When a `SPILL_LOAD` is
assigned to a use, the containing block is marked available in a bitset.
Subsequent uses on paths through that block skip the reload
(`needs_spill_reload()` returns false).

The emitter has a "dead load" optimization: `ir_emit_load_int()` (line 33356)
returns immediately without emitting any code when `use_lists[def].count == 1`,
meaning the LOAD's data output is unused (only the memory ordering token `l_N`
feeds the next instruction). On x86, load-load ordering is guaranteed by TSO,
so the load is unnecessary.

The bug: when a SPILL_LOAD was assigned to an operand of such a dead LOAD
(through a fused ADD address computation), the register allocator marked the
block as available, but the emitter silently skipped the entire LOAD -- including
the spill reload. All downstream uses on that CFG path reused the register
without reload, reading a stale value.

**Concrete example** (blind-sig function 082, vreg 6 = `d_12`, register `rbp`,
spill slot `0x10(%rsp)`):

```
IR in BB17:
  d_155 = ADD(d_12, c_39);            // FUSED into d_156
  d_156, l_156 = LOAD(l_154, d_155);  // d_156 unused! only l_156 feeds forward

RA decisions for d_12's rbp sub-interval:
  ref=54  bb=3  => SPILL_LOAD  (mark BB3 available)  <-- reload emitted here
  ref=57  bb=3  => REUSE       (BB3 available)
  ...
  ref=144 bb=13 => REUSE       (reachable from BB3)
  ref=156 bb=17 => SPILL_LOAD  (mark BB17 available) <-- reload NOT emitted!
  ref=203 bb=27 => REUSE       (BB17 available)      <-- stale rbp used -> crash
```

The SPILL_LOAD at ref=156 was set on the fused ADD (instruction 155), but
`ir_emit_load_int` returned early at the dead-load check for instruction 156,
never processing the fused address operand or its spill reload.

Assembly (before fix): only **one** `rbp` reload (BB3 path), none on the
BB2->BB15->BB17->BB27 path:

```asm
movq 0x10(%rsp), %rbp   ; line 43: reload on BB3 path
...
.L13:                    ; BB27 path -- NO reload
  movb (%rdx, %rbp), %r8b  ; rbp = stale value -> SIGSEGV
```

### Why O1-Only

- **O0:** No register allocation (no spilling), so the bug cannot manifest.
- **O1:** GCM + linear scan regalloc + scheduling. The specific pattern of a
  dead LOAD with a spilled address operand on an alternate CFG path triggers the
  bug.
- **O2:** SCCP runs first and simplifies the IR enough (constant propagation,
  dead code elimination) to avoid the dead-load-with-spill pattern entirely.

### Fix

**File:** `thirdparty/ir/ir_ra.c`

Added `ir_is_dead_load()` helper that checks if an instruction is a
LOAD/LOAD_v/VLOAD/VLOAD_v with `use_lists[ref].count == 1` -- the exact
condition the emitter uses to skip dead loads.

In `assign_regs()`, the available-marking condition now checks:

```c
if (ir_ival_covers(ival, IR_SAVE_LIVE_POS_FROM_REF(ctx->cfg_blocks[use_b].end))
 && !ir_is_dead_load(ctx, ref)) {
    ir_bitset_incl(available, use_b);
}
```

When the instruction at the use site is a dead load, the block is not marked
available, forcing downstream uses to reload from the spill slot. This adds
reloads only on paths that previously had missing reloads.

Assembly (after fix): **three** `rbp` reloads -- one per CFG path that uses it:

```asm
movq 0x10(%rsp), %rbp   ; line 43:  BB3 path (unchanged)
movq 0x10(%rsp), %rbp   ; line 241: BB27 path via .L13 (NEW)
movq 0x10(%rsp), %rbp   ; line 260: BB34 path via .L15 (NEW)
```

---

## Bug 6: LSRA Mid-Loop Register Reassignment (rust-compression O1)

**Status: OPEN** — See [current_bug.md](current_bug.md) for full details.

LSRA splits the MemoryBase interval inside a loop and reassigns `%r13` to
FuncTablePtr. On loop back-edges, no resolution move restores MemoryBase,
corrupting wasm linear memory accesses. Miscompiled function: compile_index 63.

---

## Bug 7: rust-compression O2 Regression

**Status: OPEN**

### Summary

`rust-compression` hits `unreachable` trap (error 0x40a) at O2. Passes at O0
and O1.

### Failing Call Chain (at crash time)

```
funcIdx 592 -> 572 -> 466 -> 441 -> 442 -> 474 -> 478 -> 487 (unreachable trap)
```

This chain does NOT include the suspect functions (577/578/615). The bug
corrupts **wasm linear memory**; downstream functions read wrong values and
branch to the trap stub (funcIdx 487).

### Suspect Functions

Skipping any one of these (via `WASMEDGE_IR_JIT_SKIP`) fixes the bug:

| funcIdx | func_id | Wasm params | Role |
|---------|---------|-------------|------|
| 577 | 563 | 3 x i32 | `itoa`-like: converts number to digit bytes in memory. Calls 578 with 6 wasm args via register ABI (`CALL/7` including exec_env). |
| 578 | 564 | 6 x i32 | Large function with SWITCH, multiple `call_indirect` trampolines, and 3 direct calls to funcIdx 615 via register ABI (`CALL/6`). |
| 615 | 601 | 5 x i32 | Small helper with 2 `call_indirect` paths. Called **only** from 578. |

### func_id <-> funcIdx Mapping

`ImportFuncNum = 12`, 2 functions skipped (funcIdx 487 at CodeIdx 475, funcIdx
512 at CodeIdx 500). For func_id N: `funcIdx = N + 12 + (number of skips with
CodeIdx <= N + skips)`.

### What Was Ruled Out

- **Parameter count mismatch** -- Caller/callee agree on param counts for all
  three functions.
- **7th arg (stack-passed) alignment** -- Correctly located at `0xb0(%rsp)` in
  the callee (6 pushes x 8 + `sub $0x78` + return address = 0xb0).
- **Return type mismatch** (caller PROTO declares `IR_I64`, callee compiled as
  `IR_I32`) -- Changing the caller's PROTO to `IR_I32` to match the callee did
  NOT fix the bug. The TRUNC in the disassembly correctly uses `test %eax,%eax`
  (32-bit), not `test %rax,%rax`.
- **Prologue register saves** -- All callee-saved registers properly saved.
  Short-lived params (e.g. p1/EDX in funcIdx 615) consumed before clobbering.
- **ALLOCA overlap with spill slots** -- ALLOCA at RSP+0 (24 bytes), spills
  start at RSP+0x18 -- no overlap.
- **Forcing all calls to buffer-based ABI** -- Broke unrelated functions
  (`ir_check` assertion: `SharedCallArgs` not allocated for functions that had
  no buffer-based calls originally).

### Key Observations

- **O0: PASS, O1: PASS, O2: FAIL** -- The IR framework's O2-specific
  optimizations (likely LSRA register allocation or instruction scheduling)
  produce incorrect codegen for one of these functions.
- The `thirdparty/ir` library has had multiple recent O2 regalloc fixes (TRUNC
  stale bits, LSRA eviction, dead PHI clobbering, stale EFLAGS) -- this may be
  another instance of a similar class of bug in the IR backend.
- funcIdx 615 is called **only** from funcIdx 578. Skipping 615 prevents the
  crash -- but 615 is only reached when 578 takes a specific conditional path
  (`d_152 = ULE(d_146, d_140)` in BB23). During correct execution this path may
  not be taken; wrong values computed at O2 could flip the condition.

### Current Key Findings

**ALLOCA-overlap hypothesis disproven.** Earlier analysis suspected the
SharedCallArgs ALLOCA buffer at `%rsp` overlapped with spill slots. This was
wrong:

- The ALLOCA for funcIdx 474 is **16 bytes** (c_13 = 16, i.e., MaxCallArgs = 2).
  Only 2 values are stored to the buffer before the `jit_call_indirect` call.
- `ecx = 7` is the **typeIdx** argument to `jit_call_indirect`, NOT the number
  of args in the buffer.
- `rsp+0x24` is **NOT a spill slot** -- it is the **parameter p3 save** from the
  function prologue (`mov %r8d, 0x24(%rsp)` at wasm_jit_462+50). The IR
  framework's static ALLOCA and spill allocations do not overlap.

**Actual mechanism:** funcIdx 474 (wasm_jit_462) branches on its 5th wasm
parameter `d_6` ("p3"):

```
l_263 = IF(l_262, d_6);       // IR line 322 -- branch on p3
l_264 = IF_TRUE(l_263);       // p3 != 0 -> call funcIdx 478
l_265 = END(l_264);
l_266 = IF_FALSE(l_263);      // p3 == 0 -> normal path
```

At O2, the caller (funcIdx 454 = wasm_jit_442) passes **p3 = 1**. At O0,
funcIdx 474 is **never called with p3 != 0** (confirmed via conditional GDB
breakpoint on `$r8d != 0` at wasm_jit_462 entry -- no hit at O0).

The p3 value comes from a **byte loaded from wasm linear memory** by funcIdx 454
(double indirection: load a wasm pointer from memory, then load a byte at
pointer + 8). This means some **earlier O2-compiled function writes wrong data
to wasm memory**, and the corruption cascades through the call chain.

**Binary search approach (started, not completed):** Skipping funcIdx ranges
via `WASMEDGE_IR_JIT_SKIP` to isolate the buggy function. Initial attempts with
large ranges (half the functions) produced core dumps rather than clean
pass/fail, suggesting the approach needs refinement (e.g., smaller ranges, or
only skip functions not in the critical call path).

### Known Contributing IR Backend Bugs

Two upstream IR backend bugs were identified in this function's crash path.
Fixes were attempted but reverted because they caused regressions in blake3-scalar
and shootout-ed25519. The root cause analysis is correct; the fixes need better
implementations:

- **stk_tmp=RAX clobbers fused call target** (`ir_x86.dasc`): In functions with
  >6 args AND a fused memory call target (`call *offset(%rax)`), the Bug #1 RAX
  scratch clobbers the fused base register. Only affects `wasm_jit_330` among
  sightglass kernels. Attempted R11 fix clobbered the function table pointer.
- **Within-block CALL clobbers caller-saved register** (`ir_ra.c`): When a
  spilled variable is used at two different CALLs in the same basic block, the
  `prev_use_ref` optimization skips reload for the second use. Affects
  `wasm_jit_070`. Attempted scan-based fix destabilized LSRA elsewhere.

### Suggested Next Steps

1. **Hardware watchpoint** -- Set a GDB hardware watchpoint on the exact wasm
   memory byte that funcIdx 454 reads as 1 (should be 0). This would directly
   identify which O2-compiled function writes the wrong value.
2. **Refined binary search** -- Use `WASMEDGE_IR_JIT_SKIP` with smaller ranges
   (quarters, then eighths) to isolate the one function whose O2 codegen
   corrupts wasm memory. Avoid skipping functions in the critical call path
   (442, 454, 474, 478).
3. **Suspect functions still relevant** -- Skipping funcIdx 577, 578, or 615
   individually still fixes the crash. These functions involve `call_indirect`
   and complex control flow (SWITCH) -- prime candidates for O2 regalloc bugs.
4. **Revisit the two upstream bugs** -- For the RAX/fused-target bug, find a
   register guaranteed free during call emission (not R11). For the within-block
   CALL bug, integrate CALL-awareness into `needs_spill_reload()` itself rather
   than patching the caller.

### Dump File Locations

| File | Content |
|------|---------|
| `/tmp/wasmedge_ir_563_after.ir` | funcIdx 577 O2 IR |
| `/tmp/wasmedge_ir_564_after.ir` | funcIdx 578 O2 IR |
| `/tmp/wasmedge_ir_601_after.ir` | funcIdx 615 O2 IR |
| `/tmp/wasm_jit_563_disas.txt` | funcIdx 577 O2 x86 disassembly |
| `/tmp/wasm_jit_564_disas.txt` | funcIdx 578 O2 x86 disassembly |
| `/tmp/wasm_jit_601_disas.txt` | funcIdx 615 O2 x86 disassembly |

---

## Bug 8: Narrow Spill Reload Wrongly Satisfies Wider Coalesced Use

**Status: FIXED**

### Syndrome

`sightglass-strong/sqlite3` deterministically SIGSEGVs at IR JIT O2 under
`WASMEDGE_TIER2_ENABLE=1` + `WASMEDGE_OSR_THRESHOLD=0`:

```bash
WASMEDGE_SIGHTGLASS_DIR=sightglass-strong \
WASMEDGE_SIGHTGLASS_KERNEL=sqlite3 \
WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
WASMEDGE_TIER2_ENABLE=1 \
WASMEDGE_TIER2_THRESHOLD=10 \
WASMEDGE_OSR_THRESHOLD=0 \
./build/test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

5/5 runs SIGSEGV. Any IR-shape perturbation around loops or memory
accesses (`WASMEDGE_OSR_THRESHOLD≥1` *or* `WASMEDGE_IR_JIT_BOUND_CHECK=1`)
masks it — those knobs add `LOAD`/`STORE`/`CALL` ops that shift LSRA
live-range starts and ends through the loop body, either avoiding the
bad coalescing or landing the buggy availability decision on an
innocuous interval. Neither path is a real fix.

The crash surfaced in leaf functions `wasm_jit_2552` and `wasm_jit_1818`
(host stack overflow / wasm `__stack_pointer` wrap respectively), but
those functions were *not* miscompiled — their post-opt IR and native
were byte-identical between passing and failing configs. The actual
miscompile was upstream in sqlite3 FuncIdx 1482, whose corrupted compare
made a recursion base case unreachable and leaked wasm SP across
repeated calls until it wrapped below zero.

### Root Cause

When an i32 SSA value is coalesced with a `TRUNC(i32)→u8` sub-use, the
narrow byte reload (`movb spill, %cl`) was recorded as having satisfied
the wider use too. A later same-block `cmpl %ecx, …` then consumed the
register as i32 with **stale upper bits** left over after an
intervening caller-saved-clobbering CALL.

The LSRA tracks "this block already has a spill reload available" via
the `available` bitset and `prev_use_ref`. The check that gated those
updates did not consider the *type width* of the satisfying use. A
`movb` reload followed by a `cmpl` of the same vreg looks correct at
the IR level (both read the same vreg), but the byte load leaves bits
8–31 of the register untouched. A CALL between the two reads can — and
in FuncIdx 1482 did — trash those upper bits.

### Fix

`thirdparty/ir/ir_ra.c`. New helper `ir_spill_load_preserves_interval`
returns false when an integer use is narrower than the coalesced
interval:

```c
static bool ir_spill_load_preserves_interval(ir_ctx *ctx,
    ir_live_interval *ival, ir_use_pos *use_pos)
{
    /* … */
    input_type = ctx->ir_base[input].type;
    if (IR_IS_TYPE_INT(ival->type)
     && IR_IS_TYPE_INT(input_type)
     && ir_type_size[input_type] < ir_type_size[ival->type]) {
        return 0;  /* narrow use of wider coalesced interval */
    }
    return 1;
}
```

Called from `assign_regs()` at the spill-load decision point:

```c
bool preserves_interval =
    ir_spill_load_preserves_interval(ctx, top_ival, use_pos);

if (ir_ival_covers(ival, …) && preserves_interval && !ir_is_dead_load(...)) {
    ir_bitset_incl(available, use_b);
}
prev_use_ref = preserves_interval ? ref : IR_UNUSED;
```

Two effects when the helper returns false:

1. The current block is not added to `available`, so successor blocks
   correctly see the wider value as needing a fresh spill load.
2. `prev_use_ref` is set to `IR_UNUSED`, so subsequent same-block uses
   don't treat the narrow `movb` reload as having satisfied a wider
   read.

Both are necessary. (2) catches the same-block case that broke FuncIdx
1482 — a `cmpl %ecx, %esi` immediately after a `movb` reload of the
same vreg. (1) catches cross-block analogues where the wider read is
in a successor of the byte-reload block.

This is in the same family as Bug 4 (`prev_use_ref` skipping spill
reload for same-instruction duplicate args) and Bug 5 (dead load
skipping spill reload across CFG paths): all three are LSRA decisions
that conflate "we already loaded this vreg" with "the loaded bits
satisfy every subsequent use of this vreg". Each bug refines the
condition under which the optimization is sound.

### Verification

| Check | Result |
|---|---|
| `tests/x86_64/ra_016.irt` (new) | passes with fix, fails without |
| Full `tests/x86_64/` suite (171 tests) | 171/171 pass |
| Original sqlite3 repro, 5/5 runs | all exit 0 |
| Asm diff (buggy → fixed) | exactly `+movl 0x1c(%rsp), %ecx` before `cmpl %ecx, %esi` |
