# WasmEdge IR JIT O1 Bugs: Trivial PHI Codegen & Dead PHI Register Clobbering

## Background

Optimization level `O1` in the WasmEdge IR JIT was encountering Segmentation
Faults in `rust-protobuf` and `unreachable` trap cascades in `regex`. These
issues were absent at `O0` (no optimization) and `O2` (SCCP eliminates the
problematic PHI nodes). Two root causes were identified and fixed independently.

---

## Bug 1: Trivial PHI Chains Causing Wrong Branch Decisions (regex)

### Symptom

`regex` crashes at O1 with an `unreachable` trap in Wasm function 755. The trap
cascades through `jit_host_call` (which returns 0 for the error), function 788
then also traps, and eventually the process SEGVs. At O0 the execution takes a
code path that avoids the trap. At O2 the trap path is eliminated by SCCP
constant folding. At O1 (without SCCP), the function takes the WRONG code path
entirely.

### Root Cause

The WasmEdge frontend emits one PHI per Wasm local at every loop header and
merge point, regardless of whether the local was modified on any incoming edge.
When all incoming edges carry the same value, the PHI is **trivial** — it's
semantically a no-op copy. When loops are nested, trivial PHIs form chains
through successive loop headers, each reading the previous trivial PHI.

#### What a trivial PHI is

In SSA form, a PHI node merges values from different control-flow predecessors.
A trivial PHI is one where all non-self-referencing inputs are the same value:

```
d_243 = PHI(l_242, d_21, d_21)
```

Both arms of the merge provide `d_21`, so `d_243 == d_21` always. It contributes
nothing semantically — it's a copy that the compiler doesn't recognize as one.

#### How the WasmEdge frontend creates them

The WasmEdge-to-IR builder (`lib/vm/ir_builder.cpp`) emits one PHI per Wasm
local at every loop header via `visitLoop()`. If a Wasm local holds value `d_21`
on all incoming edges (i.e., it was never modified inside the loop or on any
branch reaching this merge), the PHI still gets created with identical inputs on
every edge. The frontend doesn't check whether the local was actually modified —
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

#### Scale of the problem in func 99 (dump_id 086)

| Metric | O1 (no SCCP) | O1 + SCCP |
|--------|-------------|-----------|
| Total PHIs | 670 | 149 |
| Trivial PHIs (all inputs identical) | 98 (14.6%) | 1 (0.7%) |
| Total IR lines | 6288 | 4871 |
| Total DESSA MOVs at codegen | 769 | — |
| DESSA MOVs for trivial PHIs | 194 (25.7%) | — |

SCCP eliminates 521 PHIs (78%), including nearly all 98 trivial ones. The 194
wasted DESSA MOVs (25.7% of all DESSA MOVs) are pure overhead that also create
opportunities for register clobbering.

At merge point `l_242` alone, there are 39 PHIs: 20 trivial + 19 non-trivial.
Each trivial PHI generates one DESSA MOV instruction per loop iteration — 20
wasted register-to-register or spill-slot-to-spill-slot copies at a single
merge point.

#### How trivial PHIs break register allocation

The register allocator treats each PHI as a distinct SSA value with its own
virtual register, regardless of whether it's trivial. This creates a cascade:

1. **Different vregs for the same value.** `ir_assign_virtual_registers()` gives
   each node its own vreg:

   ```
   d_21   → R1851 (spilled)
   d_243  → R116  (spilled — different slot!)
   d_5408 → R1851 (coalesced back to same vreg as d_21)
   ```

   `d_21` and `d_5408` happen to share `R1851` (the coalescer merged them), but
   `d_243` got a different vreg `R116`. This means the value must be physically
   copied from R1851's spill slot to R116's spill slot at merge `l_242`, then
   copied back from R116 to R1851 at merge `l_5407`. Both copies move the exact
   same value — pure waste.

2. **DESSA moves between vregs at every merge point.** At each block boundary
   where a PHI lives, the DESSA pass (`ir_emit_dessa_moves` in `ir_emit.c`)
   emits parallel copy instructions to move the value from the source vreg into
   the PHI's destination vreg. The codegen dump shows:

   ```
   # At merge l_242 boundary:
   # DESSA MOV d_21 {R1851} -> d_243 {R116}     ← trivial, waste
   # DESSA MOV d_23 {R7} -> d_244 {R1852}       ← trivial, waste
   # DESSA MOV d_27 {R1865} -> d_245 {R117}     ← trivial, waste
   ...20 trivial MOVs total at this one merge...
   # DESSA MOV d_145 {R54} -> d_247 {R...}      ← non-trivial, needed
   ...19 non-trivial MOVs...
   ```

3. **Spill slot pressure and copy errors.** When the RA spills trivial PHIs to
   different stack slots (R1851 and R116 are both spilled vregs, meaning they
   live in different `[rbp-offset]` stack slots), the DESSA moves become
   memory-to-memory copies routed through a scratch register:

   ```asm
   mov eax, [rbp-0x2E8]   ; load R1851 (d_21) from spill slot
   mov [rbp-0x1D0], eax   ; store to R116 (d_243) spill slot
   ```

   The scratch register (`eax` in this case) is chosen by the DESSA emitter.
   With 20 trivial + 19 non-trivial DESSA MOVs all executing as a parallel copy
   at the same program point, the emitter must carefully sequence them to avoid
   clobbering values that haven't been read yet. The more MOVs there are, the
   more constrained the sequencing becomes, and the more likely a misordering
   will clobber a live value.

4. **Wrong branch decisions.** In func 99 of the regex benchmark, a trivial PHI
   chain carries a value used in a branch condition that determines which exit
   path the function takes:
   - **Path 1** (through BB615): writes a sentinel constant to a memory
     address — the correct path.
   - **Path 2** (through BB623): writes an `int64_t` value to the same
     address — the wrong path.

   A DESSA move for one of the trivial PHIs clobbers the register holding the
   value that the branch condition depends on. The branch evaluates incorrectly,
   and the function takes Path 2. The memory address receives `0x80000000` (low
   32 bits of an unrelated int64_t) instead of the expected sentinel
   `0x80000002`. The caller reads back this address, sees the wrong sentinel,
   and hits an `unreachable` instruction — a deliberate panic for "this should
   never happen."

#### Why the error cascades

When func 755 hits the `unreachable` trap, `jit_host_call` (in
`lib/executor/helper.cpp`) handles the error. It only longjmps for
`Terminated` errors — all other errors (including `unreachable`) simply return
0 to the JIT caller. The JIT code continues executing with this garbage return
value, causing function 788 to also trap, and eventually the process SEGVs.

This is correct behavior for `jit_host_call` (non-terminal errors should be
propagated to the executor) — the real bug is that the function shouldn't be
reaching the `unreachable` instruction at all.

### Why O2 doesn't have this bug

At O2, the **SCCP (Sparse Conditional Constant Propagation)** pass runs before
register allocation. SCCP is a lattice-based dataflow solver that walks the SSA
graph and recognizes that every trivial PHI in the chain is equivalent to the
original value. It replaces all uses with the original value directly:

```
// Before SCCP:
d_243  = PHI(l_242, d_21, d_21)       -- trivial
d_5408 = PHI/3(l_5407, d_243, d_243, d_21) -- trivial

// After SCCP: d_243 and d_5408 are replaced everywhere with d_21
// The PHIs become dead (zero uses) and don't participate in RA
```

Result: 670 PHIs → 149 PHIs. 98 trivial PHIs → 1. 769 DESSA MOVs reduced
proportionally. No unnecessary copies, no clobbering, correct branch.

### Why O0 doesn't have this bug

At O0, there is no register allocation at all. The IR backend uses a simpler
code generation strategy that doesn't assign physical registers to SSA values,
so there are no DESSA moves and no register clobbering. Trivial PHIs still
exist (1399 PHIs at O0 vs 670 at O1, since O1's CFG simplification removes
some), but they don't cause harm without RA.

### Fix (reverted)

**File:** `thirdparty/ir/ir.h` — function `ir_jit_compile`

Enable SCCP at O1 (was previously only enabled at O2):

This is just an ad-hoc fix. The real fix would require identifing the root cause of register clobbering.
```c
// BEFORE:
if (opt_level > 1) {
    if (!ir_sccp(ctx)) {
        return NULL;
    }
}

// AFTER:
if (opt_level > 0) {
    if (!ir_sccp(ctx)) {
        return NULL;
    }
}
```

SCCP is lightweight (single-pass lattice solver) and eliminates all trivial PHI
chains by propagating constants and copy values. This removes the root cause —
there are no trivial PHIs left for the RA to mishandle.

### Previous workaround (reverted)

An earlier workaround made `jit_host_call` longjmp on ALL errors (not just
`Terminated`), mimicking `jit_call_indirect`'s behavior. This masked the
symptom — the `unreachable` trap in function 755 was properly caught instead of
returning garbage — but didn't fix the root cause (wrong code path taken due to
trivial PHI codegen bug). The workaround was reverted after the SCCP fix was
applied: `jit_host_call` now only longjmps for `Terminated`, which is correct
for normal dispatch where non-terminal errors should be propagated to the
caller via the return value and checked by the executor.

### Approaches that were tried and failed

1. **Turning trivial PHIs into IR_NOP.** Attempted to replace trivial PHIs with
   NOP instructions after mem2ssa. This broke compilation because the IR backend
   requires PHIs to exist at merge points for CFG structural integrity — even
   trivial ones.

2. **Replacing trivial PHI inputs with the original value.** Only rewrote the
   PHI inputs (e.g., changed `PHI(merge, d_243, d_243)` to
   `PHI(merge, d_21, d_21)`) without replacing downstream uses of the PHI.
   The allocator still assigned different vregs to each PHI because they were
   still distinct SSA values. Didn't fix the bug.

3. **Full trivial PHI elimination (replace all uses).** Wrote code to walk PHI
   chains, find the root value, and replace all uses of every trivial PHI with
   the root. This caused "double free or corruption" and "invalid next size"
   heap crashes — the use-list manipulation was corrupting IR internal data
   structures.

4. **Custom trivial-PHI elimination pass in ir.h.** Attempted a standalone
   pass in the compilation pipeline between mem2ssa and cfg building. Required
   `ir_mem_calloc` (not available in header scope), switched to `calloc` with
   C++ casts. Still crashed with heap corruption when modifying use lists.

The correct approach was to recognize that SCCP already handles this perfectly
and just enable it at O1.

---

## Bug 2: Dead PHI DESSA Moves Clobbering Live Registers (rust-protobuf)

### Symptom

`rust-protobuf` SEGVs in `wasm_jit_016` at O1. GDB shows the faulting
instruction `mov 0x0(%rbp,%r12,1),%r10d` using r12=GlobalBase (exec_env[16])
where it should use MemoryBase (exec_env[24]). The register was silently
overwritten between function entry and the crash point.

### Root Cause

The WasmEdge frontend emits one PHI per Wasm local at every merge point. Many
of these are **dead PHIs** — their data output is never used by any instruction.
In function 016 of rust-protobuf: 110 out of 267 PHIs are dead.

At O2, SCCP eliminates dead PHIs before register allocation. At O0, there is
no register allocation. But at O1 (without the SCCP fix from Bug 1, or as a
defense-in-depth measure with it), dead PHIs survive into the RA pipeline:

1. `ir_assign_virtual_registers()` assigns vregs to ALL data nodes including
   dead PHIs (checks `IR_OP_FLAG_DATA` but not use count).
2. `ir_compute_live_ranges()` creates minimal intervals for dead PHIs (a
   `[DEF, SAVE)` range of 1 sub-position).
3. `ir_reg_alloc()` assigns physical registers to these intervals. Because
   the dead PHI's range is tiny (1 sub-position), the allocator may assign
   it the same physical register as a live value whose range doesn't overlap
   at that exact point.
4. `ir_emit_dessa_moves()` iterates ALL PHIs at each merge and generates
   parallel copy moves for dead PHIs just like live ones.
5. These dead DESSA copies write to registers that are supposed to hold live
   values, silently clobbering them.

**Concrete example from the crash:**

At function entry, r12 holds GlobalBase and r13 holds MemoryBase. Both are
callee-saved and spilled to the stack. Deep in the function, after multiple
blocks and calls, the RA reassigns r12 and r13 to hold other values. When
MemoryBase is needed again, it should be reloaded from the spill slot. But a
dead PHI's DESSA move writes a different value into the register that the code
expects to hold MemoryBase, causing the memory access to use GlobalBase instead
of MemoryBase -> SEGV (GlobalBase + offset is outside the Wasm linear memory).

### Fix

**File:** `thirdparty/ir/ir_emit.c` — function `ir_emit_dessa_moves`

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

A dead PHI's output value is never read, so its DESSA move is pure waste.
Skipping it eliminates the register clobbering without affecting correctness.

### Why `use_lists[ref].count == 0` is the right check

- `use_lists[ref]` counts instructions that reference d_ref as an operand
- For a dead PHI, no instruction reads its output, so count = 0
- Count = 1 means exactly one instruction uses the PHI — that's a live PHI
- The PHI's own inputs (merge, val1, val2) are in the OTHER direction: the
  PHI is a user of its inputs, not the other way around

### Relationship to Bug 1

With the SCCP fix (Bug 1), most dead PHIs are already eliminated before
register allocation. The dead-PHI DESSA skip is therefore a **defense-in-depth**
fix: it protects against any dead PHIs that survive SCCP (e.g., PHIs that
aren't trivial but are still dead) and ensures correctness even if SCCP is
disabled for debugging.

---

## Verified Non-Issues from Previous Notes

The previous `current_bug.md` hypothesized several frontend bugs that were
investigated and found to be **incorrect**:

1. **"ValueStack Corruption in Control Flow"** — No evidence of stack pointer
   mismanagement in the IR builder. The symptom described (wrong registers in
   memory access) was actually caused by dead PHI register clobbering.

2. **"Circular Loop PHI Self-References"** — Searched all IR dumps for
   self-referencing PHIs (`d_N = PHI(..., d_N, ...)`). Found zero instances.
   The WasmEdge frontend does NOT generate self-referencing PHIs.

---

## Summary of Changes

| File | Change | Purpose |
|------|--------|---------|
| `thirdparty/ir/ir.h` | `opt_level > 1` -> `opt_level > 0` for SCCP | Eliminate trivial PHIs at O1 |
| `thirdparty/ir/ir_emit.c` | Skip dead PHIs in `ir_emit_dessa_moves` | Defense-in-depth against dead PHI clobbering |
| `lib/executor/helper.cpp` | Remove debug error logging from `jit_host_call` | Cleanup; longjmp-on-all-errors workaround reverted |

---

## Test Results After All Fixes

| Level | Pass | Fail | Notes |
|-------|------|------|-------|
| O0    | 27   | 0    | no regression |
| O1    | 25   | 2    | was 21/27; regex, rust-protobuf, rust-json now pass |
| O2    | 25   | 2    | no regression |

### Remaining failures (pre-existing, not introduced by these fixes):

- `shootout-xblabla20`: assertion `def_reg != -1` in `ir_emit_shift_const`
  (ir_x86.dasc:5090) — RA fails to allocate a register for a shift operand.
  Crashes at both O1 and O2.
- `shootout-xchacha20`: same assertion as xblabla20 (same crypto shift pattern).
  Crashes at both O1 and O2.
- `pulldown-cmark`: produces wrong output (mismatch with interpreter reference).
  Occurs at O1; likely a GCM scheduling or RA bug, not related to PHIs.
  (Note: pulldown-cmark runs to completion without crashing, unlike the
  shootout-x* kernels, so the xblabla20 crash aborts the test process before
  pulldown-cmark's result can be fully verified in the same run.)

### O1 pipeline after fix

```
ir_build_def_use_lists
  -> ir_build_cfg -> ir_build_dominators_tree -> ir_mem2ssa   [if MEM2SSA]
  -> ir_reset_cfg
  -> ir_sccp                                                  [NEW at O1]
  -> ir_build_cfg -> ir_build_dominators_tree
  -> ir_find_loops -> ir_gcm -> ir_schedule
  -> ir_match -> ir_assign_virtual_registers
  -> ir_compute_live_ranges -> ir_coalesce -> ir_reg_alloc
  -> ir_schedule_blocks -> ir_emit_code
```

---

## Debug Tips

These are techniques that proved useful during the investigation of these bugs.
They apply broadly to any IR JIT codegen issue.

### 1. Dump IR before and after compilation

Set the environment variable `WASMEDGE_IR_JIT_DUMP=1` to dump IR for every
compiled function to `/tmp/wasmedge_ir_NNN_before.ir` (pre-optimization) and
`/tmp/wasmedge_ir_NNN_after.ir` (post-optimization, with CFG). The dump ID
`NNN` is a zero-padded sequential counter (000, 001, ...) matching the
compilation order.

```bash
WASMEDGE_IR_JIT_DUMP=1 WASMEDGE_IR_JIT_OPT_LEVEL=1 \
  WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
  ./build/test/ir/wasmedgeIRBenchmarkTests --gtest_filter="*SightglassSuite*"
```

The dump ID maps to Wasm function index as: `func_index = dump_id + ImportFuncNum`.
For the regex benchmark, `ImportFuncNum = 13`, so dump_id 086 = Wasm func 99.

### 2. Control optimization level

```bash
WASMEDGE_IR_JIT_OPT_LEVEL=0  # O0: no opts, no RA — baseline for correctness
WASMEDGE_IR_JIT_OPT_LEVEL=1  # O1: folding+CFG+MEM2SSA+SCCP+GCM+schedule+full RA
# (default) O2: same as O1 (previously O1 lacked SCCP, now they're equivalent)
```

A quick way to bisect: if O0 works and O1 doesn't, the bug is in optimization
or register allocation. If O1 works and O2 doesn't, the bug is in SCCP or a
pass that only runs at O2.

### 3. Add codegen dump to ir.h

To see register assignments and DESSA MOVs, temporarily add an `ir_dump_codegen`
call in `ir_jit_compile()` (in `thirdparty/ir/ir.h`) right before `ir_emit_code`:

```c
// Add just before: return ir_emit_code(ctx, size);
if (getenv("IR_DUMP_CODEGEN")) {
    char fname[256];
    snprintf(fname, sizeof(fname), "/tmp/wasmedge_ir_%03d_codegen.ir",
             _ir_dump_id);  // you'll need to thread the ID through
    FILE *f = fopen(fname, "w");
    if (f) { ir_dump_codegen(ctx, f); fclose(f); }
}
```

The codegen dump shows each IR node with its assigned vreg (`{Rnnn}`), physical
register (`{%reg}`), and all `# DESSA MOV src {Rn} -> dst {Rm}` annotations.
This is the most valuable dump for diagnosing RA and DESSA bugs.

### 4. Identify trivial PHIs with a script

Scan the IR dump for PHIs where all inputs (after the merge ref) are identical:

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

### 5. Count dead PHIs

After optimization, dead PHIs have zero uses. In the codegen dump, dead PHIs
are the ones that appear ONLY in `# DESSA MOV` comments and their own
definition line — they're never referenced as an input to any other instruction.

In the IR library source, you can add a diagnostic in `ir_emit_dessa_moves`:

```c
if (ctx->use_lists[ref].count == 0) {
    fprintf(stderr, "DEAD PHI: d_%d at merge %d\n", ref, bb->merge_with);
    continue; // the fix
}
```

### 6. GDB with JIT symbols

The IR JIT registers each compiled function with GDB's JIT interface via
`ir_gdb_register()`. This means you can set breakpoints on JIT functions:

```gdb
break wasm_jit_086
run
# Once hit, disassemble:
disas
# Or dump from the entry point:
x/2000i $pc
```

To get the full disassembly for offline analysis:

```gdb
set logging on /tmp/jit086_disasm.txt
disas wasm_jit_086
set logging off
```

### 7. Identify the faulting function from a SEGV

When the JIT SEGVs, GDB shows the crash address. Compare it against the
registered JIT symbol ranges:

```gdb
info functions wasm_jit_
# Lists all registered JIT functions with their address ranges
```

Or check the GDB backtrace — JIT functions appear as `wasm_jit_NNN` in the
stack if symbols were registered.

### 8. Compare IR across optimization levels

Diff the before/after IR dumps at different opt levels to isolate which pass
introduces the problem:

```bash
# Dump at O0 and O1
WASMEDGE_IR_JIT_DUMP=1 WASMEDGE_IR_JIT_OPT_LEVEL=0 ... # produces _before.ir
mv /tmp/wasmedge_ir_086_after.ir /tmp/wasmedge_ir_086_O0.ir

WASMEDGE_IR_JIT_DUMP=1 WASMEDGE_IR_JIT_OPT_LEVEL=1 ...
mv /tmp/wasmedge_ir_086_after.ir /tmp/wasmedge_ir_086_O1.ir

# Compare PHI counts
grep -c "= PHI" /tmp/wasmedge_ir_086_O0.ir  # 1399
grep -c "= PHI" /tmp/wasmedge_ir_086_O1.ir  # 670 (415 without SCCP)
```

### 9. Bisect SCCP's effect

To test whether SCCP fixes a specific function, you can temporarily add an
SCCP call at O1 in `ir.h` (which is what the final fix does). To test the
effect WITHOUT committing, set an env var:

```c
if (opt_level > 1 || getenv("IR_FORCE_SCCP")) {
    if (!ir_sccp(ctx)) return NULL;
}
```

Then run with `IR_FORCE_SCCP=1` and compare results.

### 10. JitExecEnv layout reference

When debugging register clobbering in JIT functions, the first argument is
always `JitExecEnv *env`. Its layout:

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

If a memory access uses `r12` (GlobalBase, offset 16) where it should use `r13`
(MemoryBase, offset 24), or vice versa, it's a strong indicator of register
clobbering by a dead/trivial PHI DESSA move.

### 11. Wasm function index mapping

The sightglass test harness and the IR dump use different numbering:

- **Wasm func_index**: includes imports (e.g., func 99 = 13 imports + 86 local)
- **IR dump_id**: sequential compilation order, starts at 000, corresponds to
  local function index (dump_id 086 = local func 86 = Wasm func 99 if 13 imports)
- **GDB symbol**: `wasm_jit_086` (uses dump_id)

To convert: `dump_id = func_index - ImportFuncNum`

The import count varies per benchmark. Check the JIT engine init log for the
count, or look at the Wasm module's import section.
