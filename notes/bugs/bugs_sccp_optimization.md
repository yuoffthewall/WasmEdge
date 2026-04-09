# IR Backend Bugs: SCCP & Optimization Passes

Bugs in the dstogov/ir library's optimization passes (ir_sccp.c, ir.h).

---

## Bug 1: Induction Variable Promotion Breaks Wasm 32-bit Wrapping

**Status: FIXED (proper fix, pass re-enabled with a semantic guard)**

### Syndrome
The Sightglass `quicksort` benchmark failed consistently under `IR_JIT` at O2,
crashing with an ASan SEGV on a load at an address roughly ~4 GB past the
native Wasm memory boundary (e.g. `LOAD(MemoryBase + 0xFFFFFFFC)` extended out
to an i64 address). O0/O1 runs were fine — the crash only appeared once the
SCCP pipeline ran.

### Root cause
`dstogov/ir`'s SCCP pass `ir_try_promote_induction_var_ext` walked patterns of
the form

    phi_i32 = PHI(init, add_i32)
    add_i32 = ADD_I32(phi_i32, loop_invariant)
    ...  zext_i64 = ZEXT(phi_i32)  (or of add_i32)

and, when a ZEXT of the induction variable was found, promoted the PHI and
its back-edge op to the wider type, then **dropped the ZEXT node entirely**,
assuming the wider arithmetic would give the same value as `zext(i32 value)`.

That assumption holds only while the loop stays inside `[0, 2^32)`. WebAssembly
requires strict modulo-2^32 wrap on `i32.add` / `i32.sub`, and quicksort walks
pointers downward through a slice; when the arithmetic momentarily crosses
zero (e.g. a `-4` step), the two domains diverge:

- i32 then ZEXT: `0 - 4 = 0xFFFFFFFC`, zero-extends to `0x00000000FFFFFFFC`
- i64 promoted (ZEXT dropped): `0 - 4 = 0xFFFFFFFFFFFFFFFC`

The generated load then used the i64 value directly against `MemoryBase`,
blowing past the allocation and trapping.

(An equivalent issue exists in principle for SEXT across i32 signed overflow,
but nothing in the Sightglass suite currently exercises it.)

### Why our lowering feeds ZEXT to every memory op
WebAssembly linear memory is indexed by **32-bit** values; the host pointer
is 64-bit, so the 32-bit index must be zero-extended before being added to
`MemoryBase`. The WasmEdge IR builder keeps addresses as i32 on the stack and
does the extension at each memory op in `buildMemoryAddress`
(`lib/vm/ir_builder.cpp`):

```cpp
ir_ref WasmToIRBuilder::buildMemoryAddress(ir_ref Base, uint32_t Offset) {
  ir_ctx *ctx = &Ctx;
  ir_ref WasmAddr = Base;
  if (Offset != 0) {
    WasmAddr = ir_ADD_I32(Base, ir_CONST_I32(static_cast<int32_t>(Offset)));
  }
  ir_ref WasmAddrExt = ir_ZEXT_A(WasmAddr);                 // 32-bit -> addr width
  ir_ref EffectiveAddr = ir_ADD_A(MemoryBase, WasmAddrExt); // + MemoryBase
  return EffectiveAddr;
}
```

So every load/store emits a ZEXT of whatever i32 value is on the Wasm stack.
Pointer arithmetic stays in i32 to preserve wrap semantics, and the ZEXT
appears at the pointer-formation boundary — not as "an extra optimization
opportunity" but as a correctness requirement. Hoisting the ZEXT out of the
loop and widening the IV is precisely what breaks the contract.

### Previous workaround (now superseded)
The earlier workaround was to `return 0` unconditionally at the top of
`ir_try_promote_induction_var_ext`, disabling the pass. That was correct but
coarse: every ZEXT-of-IV site across the module lost the optimization, even
loops that would never wrap.

### Proper fix

**File:** `thirdparty/ir/ir_sccp.c`, `ir_try_promote_induction_var_ext`.

Instead of dropping the hoisted ZEXT, we replace it with an explicit
`AND wide_src, 0xFFFFFFFF` mask. That preserves the
`zext(low 32 bits of the promoted IV)` semantics for every ZEXT user:
`(phi_i64) & 0xFFFFFFFF == zext(phi_i32)` regardless of whether the i64
arithmetic wraps or underflows, so the promotion is now sound even for
decrementing pointers.

Key points of the change:

1. **Gate the re-enable to `IR_ZEXT` from an i32 source.** SEXT and
   narrower sources take the existing conservative `return 0` path
   because they need a different mask / sign-extension and aren't
   exercised by any regression in the suite.
2. **Retype the PHI and back-edge op *before* constructing the mask.**
   The newly-emitted AND inherits its operand's type, and if the AND is
   emitted before `phi_insn->type = type`, the operand is still i32 and
   the AND's i64 type is inconsistent with it. Doing the retype first
   avoids the transient mismatch.
3. **Emit the AND via `IR_OPTX(IR_AND, type, 2)`, not `IR_OPT(IR_AND, type)`.**
   `IR_OPT` only encodes op+type in the low 16 bits of `optx` and leaves
   the upper 16 bits (`inputs_count`) as garbage. GCM iterates operands
   via `insn->inputs_count` in `ir_gcm_schedule_early`, so an uninitialised
   count causes it to walk off the ends and trip the
   "Early placement doesn't dominate the late" assertion at
   `ir_gcm.c:536`. `IR_OPTX(..., 2)` is the same convention `ir_ext_ref`
   uses when building 1-operand nodes (`ir_sccp.c:1974`).
4. **Bookkeeping:** `ir_use_list_add(wide_src, and_ref)` to record the new
   edge (constants aren't tracked), then `ir_bitqueue_grow` + `ir_bitqueue_add`
   so SCCP reprocesses the AND, then `ir_iter_replace_insn(ext_ref, and_ref)`
   to redirect former ZEXT users onto the AND and NOP the ZEXT. Both
   `ext_ref` and the secondary `ext_ref_2` (when present) get the same
   treatment.

Shape of the replacement (see the function for the full code):

```c
ctx->ir_base[op_ref].type = type;
phi_insn = &ctx->ir_base[phi_ref];
phi_insn->type = type;
/* ... extend phi's init-edge as before ... */

ir_val mask_val; mask_val.u64 = 0xFFFFFFFFu;
ir_ref mask_ref = ir_const(ctx, mask_val, type);

ir_ref wide_src = ctx->ir_base[ext_ref].op1;
ir_ref and_ref  = ir_emit2(ctx, IR_OPTX(IR_AND, type, 2), wide_src, mask_ref);
ir_use_list_add(ctx, wide_src, and_ref);
ir_bitqueue_grow(worklist, and_ref + 1);
ir_bitqueue_add(worklist, and_ref);
ir_iter_replace_insn(ctx, ext_ref, and_ref, worklist);
/* same block for ext_ref_2 when set */
```

### Runtime cost
`AND r64, 0xFFFFFFFF` lowers on x86-64 to a zero-extending 32-bit move,
which is effectively a register rename on modern cores. In the inner loop
we trade one `ZEXT` node for one `AND` node — no worse than the pre-
promotion form — while the loop's arithmetic now runs in the wider type,
which keeps the promotion's downstream benefits (register allocation, no
separate narrow-phi) available. For ZEXTs of non-IV addresses (the bulk of
the 15k ZEXT sites in rust-compression) the pass never fires, so this fix
does not by itself close the broader "ZEXT+ADD on every memory op" gap
called out in `notes/performance/rust-compression.md`; addressing-mode
fusion in the x86 backend is the right lever for that.

### Verification
- `quicksort` Sightglass kernel at `IR_JIT` O2: passes.
- Full Sightglass sweep (all kernels except `spidermonkey`/`tinygo`) at
  `IR_JIT` O2: no hits for `dumped|failed|error|mismatch` in the run log.

---

## Bug 2: Promotion Use-List Corruption (ir_sccp.c)

**Status: FIXED**

### The Optimization

During SCCP `ir_iter_opt`, TRUNC instructions are eliminated by
promoting their source tree to the narrower type. When the source is a
PHI, `ir_promote_i2i` (`ir_sccp.c:1799`) recurses into each PHI slot.
If a slot contains a ZEXT/SEXT/TRUNC whose source already matches the
target type, the extension node is stripped: its use-list entry for the
PHI is removed, and if no other users remain, the node is NOP'd.

The same logic exists in `ir_promote_d2f` (`ir_sccp.c:1598`) and
`ir_promote_f2d` (`ir_sccp.c:1658`) for floating-point promotions.

### The Bug

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

### Concrete Impact in pulldown-cmark

Two functions are affected (identified by instrumentation):

| Function (by insns_count) | Corrupted node | PHI ref | Slots affected | Type |
|---------------------------|---------------|---------|----------------|------|
| 2132 insns                | ZEXT ref=38 (4 uses) | PHI 1889 | 3 of 4 get zero | IR_U8 |
| 1176 insns                | ZEXT ref=34 (4 uses) | PHI 1159 | 3 of 4 get zero | IR_U8 |

Total: 6 corrupted PHI slots producing zero constants.

The corrupted zeros flow through: `PHI -> TRUNC -> STORE` -- writing
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
anyway -- they added uses pointing at the PHI for nodes that had already
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

## Bug 3: SCCP Heap-Use-After-Free on Constant-Pool Realloc

**Status: FIXED (proper fix applied in ir_sccp.c; earlier workaround
of pre-allocating a larger constant pool is no longer load-bearing)**

### Syndrome
Two distinct assertion failures seen in practice, both secondary
symptoms of the same heap-use-after-free that corrupts the IR graph:

1. `ir_cfg.c:305: ir_build_cfg: Assertion '((ir_op_flags[insn->op] & (1<<12)) != 0)' failed`
   during `ir_jit_compile` at O2 — `ir_build_cfg` encounters a
   non-BB-start instruction where it expects one.
2. `ir_sccp.c:1916: ir_promote_i2i: Assertion '0' failed` and
   `ir_private.h:1067: ir_next_control: Assertion '0' failed`, observed
   on pulldown-cmark after reshaping `visitBrTable` to use
   `ir_SWITCH` + `ir_CASE_VAL`/`ir_CASE_DEFAULT`.

All only reproduce at `-O2` (SCCP runs); O0/O1 pass. The crash is
non-deterministic and depends on heap layout — setting the unrelated
env var `WASMEDGE_SIGHTGLASS_QUICK=1` changes allocations enough to
mask it.

### Reproducers
```
WASMEDGE_SIGHTGLASS_KERNEL=regex WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```
and the pulldown-cmark kernel under the same env.

### Root Cause (confirmed with ASan and GDB)
`ir_promote_i2i`, `ir_promote_d2f`, and `ir_promote_f2d` hold raw
`ir_insn *insn` pointers into `ctx->ir_base` across recursive calls.
Those recursive calls can create new constants via `ir_const()`. When
the constant pool is full, `ir_const()` -> `ir_next_const()` ->
`ir_grow_bottom()` calls `ir_mem_realloc()`, which **reallocates the
entire `ir_base` buffer** and updates `ctx->ir_base`. Every `insn`
pointer on the call stack is now dangling, and subsequent reads/writes
through them touch freed memory.

ASan stack trace (original reproducer, regex kernel):
```
WRITE of size 4 at 0x521000185778 thread T0
    #0 ir_promote_i2i           ir_sccp.c:1920
    #1 ir_iter_opt              ir_sccp.c:3641
    #2 ir_sccp                  ir_sccp.c:3791
    #3 ir_jit_compile           ir.h:1036

freed by thread T0 here:
    #0 __interceptor_realloc
    #1 ir_grow_bottom           ir.c:317
    #2 ir_next_const            ir.c:329
    #3 ir_const_ex              ir.c:552
    #4 ir_const                 ir.c:564
```

### Why our changes exposed it

**Direct-call change (regex, original repro).** The old code reused a
single function pointer (`DirectOrHostFn`, loaded once at function
entry) for all direct calls, requiring few constants per function. The
new code creates a unique `ir_CONST_ADDR(funcIdx * sizeof(void*))` per
call site. For large Wasm functions with many calls, this exhausts the
initial 4-entry constant pool (`IR_CONSTS_LIMIT_MIN`), triggering the
realloc during SCCP.

**SWITCH lowering (pulldown-cmark).** The old if-chain gave
`IndexVal` N uses (one `ir_EQ` per case); the new SWITCH gives it
exactly 1 use. This didn't directly cause the realloc, but it changed
the SCCP optimization landscape:

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

### Crash A — `ir_promote_i2i` PHI-loop pointer invalidation

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
iteration read from freed memory. In the pulldown-cmark run the
garbage value (67112) was far beyond `ctx->insns_count` (1943); the
instruction at that bogus address had opcode 249 (not a valid IR op),
falling through to the `default` case and hitting `IR_ASSERT(0)` at
line 1916.

GDB evidence:
```
#7  ir_promote_i2i(ctx, type=IR_U8, ref=67112, use=1700, ...)   <- garbage ref
#8  ir_promote_i2i(ctx, type=IR_U8, ref=1700, use=1742, ...)    <- PHI
#9  ir_iter_opt(ctx, worklist)                                    <- TRUNC at 1742
```
`ctx->insns_count = 1943`, so ref=67112 is wildly out of bounds, and
`ir_op_name[249]` returns NULL.

### Crash B — `ir_iter_opt` stale-`insn` writeback

After fixing Crash A, a second pulldown-cmark function crashed in
`ir_build_cfg` (which runs after SCCP). The `ir_iter_opt` call site
for TRUNC promotion wrote back through a stale `insn` pointer:

```c
case IR_TRUNC:
    if (ir_may_promote_trunc(ctx, insn->type, insn->op1)) {
        ir_ref ref = ir_promote_i2i(ctx, insn->type, insn->op1, i, worklist);
        insn->op1 = ref;   // <-- insn is stale after realloc!
        ir_iter_replace_insn(ctx, i, ref, worklist);
    }
```

`insn` pointed into the old (freed) `ir_base`. Writing `ref` (=131,
a PHI) through the dangling pointer overwrote an unrelated
instruction's field. Specifically, `END(104).op1` was corrupted from
104 to 131 (a PHI ref), so `ir_next_control` could not find a
control-flow successor for instruction 104.

GDB evidence:
```
insn[104]: op=IF_FALSE  op1=103        <- control node
insn[105]: op=END       op1=131        <- should be 104, corrupted to 131 (a PHI)
insn[131]: op=PHI       op1=130        <- data node, not a control predecessor
```

### Earlier workaround (in ir_builder.cpp)
Pre-allocate a 256-entry constant pool instead of the minimum 4:
```cpp
ir_init(&Ctx, ir_flags, 256, IR_INSNS_LIMIT_MIN);
//                       ^^^ was IR_CONSTS_LIMIT_MIN (= 4)
```
This masked the regex reproducer by avoiding the realloc during
optimization, but it only postponed the bug — the pulldown-cmark
repro later exhausted the pool again because the SWITCH lowering
burst-created many new U8 constants. The proper fix below replaces
this workaround as the load-bearing defense.

### Proper fix (in `thirdparty/ir/ir_sccp.c`, 25 insertions, 10 deletions)

Make the promotion helpers resilient to `ir_base` moving under them:

1. **Rewrite `ir_promote_i2i`'s PHI handler as index-based iteration**,
   re-deriving the address from `ctx->ir_base[ref]` each step so the
   loop never reads through a pointer that may have been freed:

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

2. **Reload `insn = &ctx->ir_base[ref]` after every recursive
   `ir_promote_*` call** in the NEG/ABS/NOT, binary-op, and COND
   handlers of `ir_promote_i2i`; same reload pattern in
   `ir_promote_d2f` and `ir_promote_f2d` for their NEG/ABS and
   binary-op handlers.

3. **Fix `ir_iter_opt` writeback sites.** Replace `insn->op1 = ref`
   with `ctx->ir_base[i].op1 = ref` in all `ir_iter_opt` promote call
   sites (FP2FP, FP2INT, TRUNC), so the write always goes through the
   current `ir_base` pointer. Also reload `insn` before
   `goto folding` in the FP2INT path.

An alternative (not taken) would be to change `ir_grow_bottom` to a
growth strategy that never moves the buffer (e.g., a linked-list of
pages); the per-caller reload approach is smaller and local to SCCP.

### Verification
All 38 sightglass kernels pass at `-O2`, including pulldown-cmark
(previously crashed) and shootout-switch (the SWITCH-lowering
optimization target). regex (the original ASan reproducer) also
passes without needing the 256-entry pre-allocation to mask the race.

---

## Bug 4: IR_OPT_INLINE Miscompilation with IF/THEN/ELSE Call Pattern

**Status: WORKAROUND**

### Syndrome
Silent wrong results (no crash, no assertion). Compression benchmarks
produce incorrect output -- e.g., bz2 outputs `compressed length: 60647`
instead of `57507`; rust-compression brotli size is `168242` instead of
`167110`.

### Reproducer (before the workaround)
An earlier iteration of the inlined call code used `ir_IF` / `ir_IF_TRUE`
/ `ir_IF_FALSE` / `ir_MERGE_2` / `ir_PHI_2` to branch on a null-check
of FuncPtr, with the fast path calling the JIT function directly and the
slow path calling `jit_host_call`. This produced correct results at O0
and O1, but wrong results at O2 (`IR_OPT_INLINE` enabled).

### Root Cause
The IR library's `IR_OPT_INLINE` pass (enabled at O2) miscompiles the
IF/THEN/ELSE pattern containing two indirect calls (one in each branch)
merged with a PHI node. The exact mechanism is unknown (likely incorrect
alias analysis or code motion across the branches), but the effect is
that memory operations (stores to the shared args buffer, loads from
memory in the callee) produce wrong values.

### Workaround (in ir_builder.cpp)
Eliminated the branching pattern entirely. For local functions
(`funcIdx >= ImportFuncNum`), the FuncTable entry is always non-null
(only unreachable trap stubs are skipped during JIT compilation), so the
null check is unnecessary. The code now does an unconditional direct
call.

### Proper Fix (upstream)
The `IR_OPT_INLINE` pass should correctly handle IF/THEN/ELSE patterns
with indirect calls and PHI merges. Likely a bug in how the pass
handles memory effects of calls in diamond control flow.
