
Summary of the root causes (details are in `notes/O0_O1_root_cause.md`):

---

## O0 – why it’s wrong

1. **`IR_OPT_CODEGEN` is turned off**  
   In `ir_jit_compile` for `opt_level == 0`, the code does:
   ```c
   ctx->flags &= ~(IR_OPT_CFG | IR_OPT_CODEGEN);
   ```
   So for the rest of compilation and in the emitter, **IR_OPT_CODEGEN is clear**.

2. **Emitter behavior**  
   In `ir_x86.dasc` / `ir_emit_x86.h`, the “good” paths are guarded by `(ctx->flags & IR_OPT_CODEGEN)`:
   - ADD with constant → LEA fusion (e.g. `IR_LEA_OB`, `IR_LEA_OB_I`, etc.)
   - Fusing LOAD(addr) with ADD for addressing

   When the flag is off, those paths are **not** taken. The emitter falls back to another path, which (as in your `ir_builder.cpp` comment) **fuses LOAD(addr) and ADD incorrectly** and generates bad addresses → segfaults.

3. **Different regalloc**  
   O0 uses `ir_compute_dessa_moves` and does **not** run the full pipeline (live ranges, coalesce, `ir_reg_alloc`, `ir_schedule_blocks`). So O0 uses a different, simpler regalloc (DESSA). That can combine badly with the already-broken codegen.

**Root cause for O0:** Turning off **IR_OPT_CODEGEN** makes the emitter skip the correct LEA/address fusion and use a **buggy fallback** for LOAD+ADD; O0’s different regalloc path doesn’t fix that.

---

## O1 – why it’s wrong (e.g. ackermann)

1. **MEM2SSA is never run**  
   At O1 the builder does **not** set `IR_OPT_MEM2SSA`. So in `ir_jit_compile` the block that runs `ir_mem2ssa(ctx)` is skipped (it’s under `if (ctx->flags & IR_OPT_MEM2SSA)`).

2. **What MEM2SSA does**  
   In `ir_mem2ssa.c`, MEM2SSA puts **IR_VAR** and **IR_ALLOCA** memory into SSA form: VLOAD/VSTORE and loads/stores from ALLOCA are converted to explicit SSA (PHI, etc.) so each load is tied to the right store.

3. **What our IR uses**  
   The WasmEdge IR builder uses **ir_ALLOCA** (e.g. `SharedCallArgs` and other temps). Without MEM2SSA, those remain as plain IR_LOAD/IR_STORE on ALLOCA.

4. **Why that breaks**  
   Later passes (e.g. **ir_gcm**, **ir_schedule**, **ir_match**) reorder and optimize. Without MEM2SSA they don’t see proper memory SSA, so they can:
   - reorder a store and a load incorrectly, or
   - treat two loads as independent when they shouldn’t be,

   giving wrong values or wrong control flow → segfaults (e.g. ackermann).

**Root cause for O1:** **IR_OPT_MEM2SSA is not set at O1**, so **ir_mem2ssa never runs**. ALLOCA (and VAR) memory stays as non-SSA; the rest of the pipeline then reorders/optimizes it incorrectly and produces wrong code.

---

## Summary table

| Level | Root cause |
|-------|------------|
| **O0** | `IR_OPT_CODEGEN` cleared → emitter uses broken LOAD+ADD / LEA path; different (DESSA) regalloc. |
| **O1** | No `IR_OPT_MEM2SSA` → no `ir_mem2ssa` → ALLOCA/VAR memory not in SSA → wrong reordering/optimization → segfaults. |
| **O2** | MEM2SSA and full codegen enabled → correct memory dependencies and addressing. |

The full write-up with pipeline steps and code references is in **`notes/O0_O1_root_cause.md`**.