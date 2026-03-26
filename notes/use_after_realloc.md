# ir_sccp.c: use-after-realloc in SCCP type promotion

## Symptom

After replacing `visitBrTable`'s linear `if (idx == i)` chain with
`ir_SWITCH` + `ir_CASE_VAL`/`ir_CASE_DEFAULT`, pulldown-cmark core
dumped with two distinct assertion failures:

1. `ir_sccp.c:1916: ir_promote_i2i: Assertion '0' failed`
2. `ir_private.h:1067: ir_next_control: Assertion '0' failed`

Both passed at `-O1` (no SCCP) and only crashed at `-O2`.

## Root cause

`ir_promote_i2i`, `ir_promote_d2f`, and `ir_promote_f2d` hold raw
`ir_insn *insn` pointers into `ctx->ir_base` across recursive calls.
Those recursive calls can create new constants via `ir_const()`.  When
the constant pool is full, `ir_const()` -> `ir_next_const()` ->
`ir_grow_bottom()` calls `ir_mem_realloc()`, which **reallocates the
entire `ir_base` buffer** and updates `ctx->ir_base`.  Every `insn`
pointer on the call stack is now dangling.

### Why the SWITCH lowering triggered it

The old if-chain gave `IndexVal` N uses (one `ir_EQ` per case).  The
new SWITCH gives it exactly 1 use.  This didn't directly cause the
realloc, but it changed the SCCP optimization landscape:

- TRUNC-promotion (`ir_may_promote_trunc` / `ir_promote_i2i`) found
  new promotion opportunities on PHI nodes with many constant inputs
  (13 cases from the br_table).
- Promoting 13 constants from `I32` to `U8` required creating new
  `U8` constants that did not previously exist in the constant pool.
- The burst of `ir_const()` calls exhausted `consts_limit` and
  triggered `ir_grow_bottom()` mid-recursion.

With the old if-chain the same PHI had fewer promotable paths (the
cascaded control flow produced different PHI structures), so the
constant pool never grew during promotion.

## Crash #1 — `ir_promote_i2i` assertion (function 081)

### Mechanism

The PHI handler in `ir_promote_i2i` iterated over inputs with a raw
pointer `p`:

```c
case IR_PHI:
    for (p = insn->ops + 2, n = insn->inputs_count - 1; n > 0; p++, n--) {
        input = *p;
        if (input != ref) {
            *p = ir_promote_i2i(ctx, type, input, ref, worklist);
            // ^^^ may realloc ir_base, invalidating p
        }
    }
```

After the recursive call reallocated `ir_base`, `*p` on the next
iteration read from freed memory.  The garbage value (67112) was far
beyond `ctx->insns_count` (1943).  The instruction at that bogus
address had opcode 249 (not a valid IR op), falling through to the
`default` case and hitting `IR_ASSERT(0)` at line 1916.

### GDB evidence

```
#7  ir_promote_i2i(ctx, type=IR_U8, ref=67112, use=1700, ...)   <- garbage ref
#8  ir_promote_i2i(ctx, type=IR_U8, ref=1700, use=1742, ...)    <- PHI
#9  ir_iter_opt(ctx, worklist)                                    <- TRUNC at 1742
```

- `ctx->insns_count = 1943`, so ref=67112 is wildly out of bounds.
- `ir_op_name[249]` returned NULL (invalid opcode).

### Fix

Replaced pointer-based iteration with index-based iteration that
re-derives the address from `ctx->ir_base[ref]` each iteration:

```c
case IR_PHI: {
    ir_ref ic = insn->inputs_count;
    ir_ref k;
    for (k = 2; k <= ic; k++) {
        input = ctx->ir_base[ref].ops[k];
        if (input != ref) {
            ctx->ir_base[ref].ops[k] = ir_promote_i2i(ctx, type, input, ref, worklist);
        }
    }
    ctx->ir_base[ref].type = type;
    return ref;
}
```

Also added `insn = &ctx->ir_base[ref]` reloads after every recursive
`ir_promote_*` call in the NEG/ABS/NOT, binary-op, and COND handlers
of all three promote functions.

## Crash #2 — `ir_next_control` assertion (function 123)

### Mechanism

After fixing crash #1, function 123 crashed in `ir_build_cfg` (which
runs after SCCP).  The `ir_iter_opt` call site for TRUNC promotion
wrote back through a stale `insn` pointer:

```c
case IR_TRUNC:
    if (ir_may_promote_trunc(ctx, insn->type, insn->op1)) {
        ir_ref ref = ir_promote_i2i(ctx, insn->type, insn->op1, i, worklist);
        insn->op1 = ref;   // <-- insn is stale after realloc!
        ir_iter_replace_insn(ctx, i, ref, worklist);
    }
```

`insn` pointed into the old (freed) `ir_base`.  Writing `ref` (=131,
a PHI) through the dangling pointer overwrote an unrelated
instruction's field.  Specifically, `END(104).op1` was corrupted from
104 to 131 (a PHI ref), so `ir_next_control` could not find a
control-flow successor for instruction 104.

### GDB evidence

```
insn[104]: op=IF_FALSE  op1=103        <- control node
insn[105]: op=END       op1=131        <- should be 104, corrupted to 131 (a PHI)
insn[131]: op=PHI       op1=130        <- data node, not a control predecessor
```

### Fix

Replaced `insn->op1 = ref` with `ctx->ir_base[i].op1 = ref` in all
`ir_iter_opt` promote call sites (FP2FP, FP2INT, TRUNC), so the
write always goes through the current `ir_base` pointer.

## Files changed

`thirdparty/ir/ir_sccp.c` — 25 insertions, 10 deletions:

- `ir_promote_i2i`: reload `insn` after recursive calls in NEG/ABS/NOT,
  binary-op, COND handlers; rewrite PHI handler as index-based loop.
- `ir_promote_d2f`: same reload pattern for NEG/ABS and binary-op handlers.
- `ir_promote_f2d`: same reload pattern for NEG/ABS and binary-op handlers.
- `ir_iter_opt`: use `ctx->ir_base[i].op1` instead of `insn->op1` for
  FP2FP, FP2INT, and TRUNC post-promote writeback; reload `insn` before
  `goto folding` in the FP2INT path.

## Verification

All 38 sightglass kernels pass at `-O2`, including pulldown-cmark
(previously crashed) and shootout-switch (the original optimization
target).
