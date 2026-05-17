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

## Bug 3: SCCP Promotion Stale Writes After Constant-Pool Realloc

**Status: FIXED**

### Syndrome

This bug has shown up as several different O2-only IR corruption
symptoms. The `tract-onnx-image-classification` reproducer trapped under
IR JIT with tier2 instrumentation enabled:

```text
[error] execution failed: unreachable, Code: 0x40a
When executing function name: "_start"
execute _start failed: tract-onnx-image-classification IR_JIT 1034
```

The same failure still reproduced with tier2 enabled but actual tier2
compilation disabled:

```sh
cd build
WASMEDGE_SIGHTGLASS_DIR=sightglass-strong \
WASMEDGE_SIGHTGLASS_KERNEL=tract-onnx-image-classification \
WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
WASMEDGE_TIER2_ENABLE=1 \
WASMEDGE_TIER2_THRESHOLD=1000000000 \
WASMEDGE_TIER2_MAX_COMPILE=0 \
stdbuf -oL timeout 120 ./test/ir/wasmedgeIRBenchmarkTests \
  --gtest_filter='*SightglassSuite*' \
  > /tmp/tract-high.log 2>&1
grep -iE 'dumped|error|failed|mismatch|warning' /tmp/tract-high.log
```

That made the failure a tier1 IR-JIT O2 bug, not a bad LLVM tier2
module.

### Root cause

`ir_promote_i2i`, `ir_promote_d2f`, and `ir_promote_f2d` recursively
promote source trees while eliminating narrowing or widening nodes. When
a promoted input is a constant, the recursive call creates a new constant
with `ir_const()`. If the constant pool is full, `ir_const()` can grow the
bottom of the IR allocation and reallocate `ctx->ir_base`.

The old code wrote recursive promotion results directly into the current
instruction:

```c
insn->op1 = ir_promote_i2i(ctx, type, insn->op1, ref, worklist);
```

and for PHI inputs:

```c
ctx->ir_base[ref].ops[k] =
    ir_promote_i2i(ctx, type, input, ref, worklist);
```

C does not require the left-hand destination address to be computed after
the right-hand call. At O2, the compiler could compute the destination
address in the old `ctx->ir_base`, then the recursive `ir_promote_*()`
call could create a constant and reallocate `ctx->ir_base`. The final
assignment then stored through a stale address instead of updating the
live IR instruction.

The bug also affected chained assignments such as:

```c
insn->op2 = insn->op1 = ir_promote_i2i(...);
```

because either destination can be based on the stale `insn` pointer.

### Evidence

The reduced reproducer is:

```text
TRUNC_U8(PHI(i32 254, i32 255))
```

with a small constant table so promoting the two PHI inputs forces
constant-table growth.

The old backend produced a partially promoted conditional:

```text
uint8_t d_9 = COND(d_2, c_254, c_257)
```

where `c_254` was still the original `int32_t` constant and `c_257` was
the new `uint8_t` constant. The fixed backend promotes both operands:

```text
uint8_t d_9 = COND(d_2, c_256, c_257)
```

The larger failing tract dump also showed stale use-list state after
SCCP/iteration:

```text
ir_base[3944] is in use list of ir_base[36]
ir_base[4101] is in use list of ir_base[36]
```

That is consistent with a stale write: the live operand graph and the
use lists no longer described the same IR.

Earlier regex and pulldown-cmark investigations exposed the same class of
bug with ASan/GDB: recursive promotion held raw `ir_insn *` pointers into
`ctx->ir_base`, while `ir_const()` could reallocate the backing buffer.

### Fix

The fix is to never write to an instruction operand across a recursive
promotion call. The promotion helpers now:

1. Read the old operand.
2. Call the recursive `ir_promote_*()` helper and store the result in a
   temporary `ir_ref`.
3. Reload `insn = &ctx->ir_base[ref]` after the recursive call.
4. Store the promoted operand into the reloaded instruction.

Fixed pattern:

```c
ir_ref promoted = ir_promote_i2i(ctx, type, insn->op1, ref, worklist);
insn = &ctx->ir_base[ref]; /* reload - ir_const() may realloc ir_base */
insn->op1 = promoted;
```

The same rule is applied to:

- unary integer promotions (`NEG`, `ABS`, `NOT`)
- binary integer promotions (`ADD`, `SUB`, `MUL`, `MIN`, `MAX`,
  `OR`, `AND`, `XOR`, `SHL`)
- `COND` value operands
- double-to-float promotion
- float-to-double promotion

The PHI path now uses operand helpers after recursion:

```c
input = ir_insn_op(&ctx->ir_base[ref], k);
if (input != ref) {
    ir_ref promoted = ir_promote_i2i(ctx, type, input, ref, worklist);
    ir_insn_set_op(&ctx->ir_base[ref], k, promoted);
}
```

The `ir_iter_opt` promotion call sites already write back through
`ctx->ir_base[i]` and reload before folding, so this patch completes the
same realloc-safe rule inside the promotion helpers themselves.

### Regression test

`IRBackendTest.SCCPPromotesPhiAfterConstTableGrowth` builds the reduced
IR shape with a deliberately tiny constant table:

```text
TRUNC_U8(PHI(i32 254, i32 255))
```

After `ir_sccp(ctx)`, the PHI must be converted into an `IR_U8`
`IR_COND` whose two value operands are both `IR_U8` constants with values
`254` and `255`.

### Verification

Every validation run redirected output to a log and grepped the log with:

```sh
grep -iE 'dumped|error|failed|mismatch|warning' LOG
```

Passing results:

| Check | Result |
|---|---|
| Standalone IR tool on the reduced failing dump at O2 | exit 0, grep found no matches |
| `tract-onnx-image-classification`, IR_JIT O2, tier2 enabled, threshold `1000000000`, max compile `0` | exit 0, grep found no matches |
| `tract-onnx-image-classification`, IR_JIT O2, original threshold `10` | exit 0, grep found no matches |
| `IRBackendTest.SCCPPromotesPhiAfterConstTableGrowth` | exit 0, grep found no matches |
| `git diff --check` and `git -C thirdparty/ir diff --check` | clean |

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
