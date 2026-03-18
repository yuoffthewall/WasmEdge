Bug Summary
Bug 2: IR library register allocator crashes during compilation of complex functions

Symptom: regex (1/907 funcs) and rust-json (1/442 funcs) segfault during
instantiate() — at compile time, not runtime.

Crash location: ir_ra.c, inside ir_compute_live_ranges(), in the linked-list
code path (the default — IR_BITSET_LIVENESS is commented out in ir_private.h).


Root cause — detailed explanation
=================================

The crash is a NULL dereference of ctx->live_intervals[v]. This array is
indexed by virtual register number. A live interval is supposed to be created
for every vreg that is live (has defs and uses). The crash means a vreg exists
(has an assigned number) but was never given a live interval.

How live intervals are created (linked-list path)
-------------------------------------------------

1. ir_compute_live_sets() runs first. It walks all vregs, finds their uses
   (including PHI uses in successor blocks), and populates live_outs[b] — a
   linked list of vregs that are live at the exit of block b.

2. The main loop (line 1281) iterates blocks in reverse order. For each block:
   a. For each vreg in live_outs[b], it calls ir_add_prev_live_range() which
      CREATES the live interval if it doesn't exist (line 1290).
   b. For PHI inputs in successor blocks, it reads ctx->live_intervals[v]
      directly (line 1316) — NO creation, just access.
   c. For each instruction def in reverse order, it calls ir_fix_live_range()
      which also assumes the interval already exists (line 1411).

So the only place intervals are created is step 2a, via live_outs. If a vreg
is missing from live_outs, its interval is never created, and steps 2b/2c crash.

Compare with the bitset path (line 694): it calls ir_add_prev_live_range() for
PHI inputs too, which creates on demand. The linked-list path is less defensive.

Why vregs go missing from live_outs
------------------------------------

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
   can become inconsistent — a PHI still references a value, but the value's
   use_list no longer includes that PHI.

4. ir_compute_live_sets iterates the stale use_list, doesn't find the PHI,
   doesn't add the vreg to live_outs. No live interval is created.

The three crash sites in ir_compute_live_ranges
-----------------------------------------------

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


Why reducing PHIs alone does NOT fix the bug
=============================================

We tried two approaches to reduce trivial PHIs:

Fix: Skip unmodified locals at back-edge (emitLoopBackEdge)
------------------------------------------------------------------
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


Why NULL guards are necessary
-----------------------------
The linked-list path in ir_compute_live_ranges has a structural gap: it assumes
ir_compute_live_sets populated live_outs correctly for every reachable vreg.
The bitset path (line 694) doesn't make this assumption — it creates intervals
on demand via ir_add_prev_live_range. The NULL guards align the linked-list
path's behavior with the bitset path, making it resilient to stale use-lists
regardless of what optimization passes do upstream.


Fix — both sides
================

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
  wrapping semantics — see notes/sccp_bug.md).

Result: All sightglass benchmarks compile with zero failures.
