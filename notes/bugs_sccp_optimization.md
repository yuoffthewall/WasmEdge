# IR Backend Bugs: SCCP & Optimization Passes

Bugs in the dstogov/ir library's optimization passes (ir_sccp.c, ir.h).

---

## Bug 1: Induction Variable Promotion Breaks Wasm 32-bit Wrapping

**Status: FIXED**

### The Problem
The Sightglass `quicksort` benchmark failed consistently under the `IR_JIT` backend, causing a crash due to an AddressSanitizer (ASan) SEGV fault. The crash reported an invalid memory access (`READ`) at an out-of-bounds address (e.g. `0x7b5659c05c7c`), effectively pointing approximately ~4GB out of bounds from the native Wasm memory boundary.

### The Root Cause
The root cause was a conflict between WebAssembly's 32-bit memory semantics and an aggressive induction variable optimization pass within the underlying `dstogov/ir` JIT engine.

1. **Wasm 32-bit Linear Memory Wrapping:** Wasm memory addresses are strictly computed as 32-bit integers. If a pointer traversing an array (like in `quicksort`) drops below zero, Wasm relies on 32-bit integer wrapping. For example, `base - 4` evaluates to `0xFFFFFFFC`. In a correct IR generation phase, this 32-bit value will eventually be zero-extended (`ir_ZEXT_A`) to a 64-bit native pointer size before being added to `MemoryBase`.
2. **Aggressive Induction Variable Promotion:** During the JIT compiler's Sparse Conditional Constant Propagation (SCCP) phase, the optimization pass `ir_try_promote_induction_var_ext` noticed that a loop variable (our 32-bit Wasm memory index) was being zero-extended on each iteration. To "optimize" this, the compiler *hoisted* the zero-extension out of the loop and promoted the induction variable up to a 64-bit integer.
3. **Broken Wrapping Semantics:** Because the induction variable arithmetic was now promoted and operating in a 64-bit domain *before* the math finished, Wasm's intrinsic 32-bit modulus semantics broke. Adding a negative offset such as `-4` (which represents the unsigned 32-bit constant `0xFFFFFFFC`) simply added `4,294,967,292` to a 64-bit base offset without wrapping cleanly at 32 bits.
4. **The Crash:** The compiled native code would generate an instruction like `LOAD(MemoryBase + 4294967292)`, blasting far outside the Wasm memory allocation and causing an immediate hardware SEGV trap.

### The Fix
Since WebAssembly requires strict 32-bit modular arithmetic compliance for any values indexing linear memory, we modified the `dstogov/ir` backend to suppress this incompatible optimization path.

**Implementation Details:**
- **Modified File:** `thirdparty/ir/ir_sccp.c`
- **Change:** Early exit/ disabled `ir_try_promote_induction_var_ext`:
  ```c
  static bool ir_try_promote_induction_var_ext(ir_ctx *ctx, ir_ref ext_ref, ir_ref phi_ref, ir_ref op_ref, ir_bitqueue *worklist)
  {
      // FAST FAIL: Disable induction variable extension promotion.
      // This optimization is unsafe for Wasm, because Wasm explicitly relies on 32-bit integer wrapping
      // (e.g. pointer decrements crossing 0) which is broken if promoted to 64-bit without explicit masking.
      return 0;
      // ...
  }
  ```

By disabling this pass, the IR preserves the original Wasm execution semantics: the arithmetic remains 32-bit, gracefully wraps, and is properly zero-extended to native width *right before* the memory load/store offsets are calculated.

The resulting fix cleared all SEGV violations and the `quicksort` IR_JIT compiling target properly executed and finished successfully.

### Why the Code Zero-Extends Every Iteration

WebAssembly linear memory is defined in terms of **32-bit** indices. On a 64-bit host, the real pointer is:

`effective_address = MemoryBase + offset`

`MemoryBase` is 64-bit; the Wasm "offset" is 32-bit. So the 32-bit value must be **zero-extended to 64-bit** at the point where you form the pointer. That's a requirement of the ABI / memory model, not a choice.

In the Wasm->IR lowering this is done in one place: **`buildMemoryAddress`** in `ir_builder.cpp`:

```1522:1538:lib/vm/ir_builder.cpp
ir_ref WasmToIRBuilder::buildMemoryAddress(ir_ref Base, uint32_t Offset) {
  ir_ctx *ctx = &Ctx; // For IR macros
  
  // Compute Wasm effective address in I32 (base from stack is Wasm i32).
  // Use I32 ops so ir_check does not see I32 vs U32 mismatch.
  ir_ref WasmAddr = Base;
  if (Offset != 0) {
    WasmAddr = ir_ADD_I32(Base, ir_CONST_I32(static_cast<int32_t>(Offset)));
  }
  
  // Zero-extend Wasm address (32-bit) to native address width
  // Then add to memory base pointer
  ir_ref WasmAddrExt = ir_ZEXT_A(WasmAddr);
  ir_ref EffectiveAddr = ir_ADD_A(MemoryBase, WasmAddrExt);
  
  return EffectiveAddr;
}
```

So:

- Address arithmetic (e.g. `Base`, or `Base + Offset`) is kept in **I32** so 32-bit wrapping is correct.
- Only when building the actual pointer do we do **ZEXT_A** (32-bit -> address width) then **ADD_A** with `MemoryBase`.

The IR builder does **not** keep a single 64-bit "memory pointer" in the loop. It keeps the Wasm index as **32-bit** (on the stack / in phis / in locals). Every **load and store** calls `buildMemoryAddress` with whatever 32-bit value is on the stack at that moment.

So the zero-extension appears **at every load/store**, i.e. "every iteration" (or more precisely, every memory operation in the loop). That's not an extra optimization; it's the direct result of:

1. Representing addresses as 32-bit until the last moment.
2. Building the real pointer only at each load/store via `buildMemoryAddress`.

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

**Status: WORKAROUND (upstream fix needed)**

### Syndrome
`ir_cfg.c:305: ir_build_cfg: Assertion '((ir_op_flags[insn->op] & (1<<12)) != 0)' failed`
during `ir_jit_compile` at O2. The assertion is a *secondary* symptom;
the real error is a heap-use-after-free that corrupts the IR graph,
causing `ir_build_cfg` to encounter a non-BB-start instruction where it
expects one. The crash is non-deterministic and depends on heap layout
(e.g., setting the unrelated env var `WASMEDGE_SIGHTGLASS_QUICK=1`
changes allocations enough to mask it).

### Reproducer (before the workaround)
```
WASMEDGE_SIGHTGLASS_KERNEL=regex WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

### Root Cause (confirmed with ASan)
The SCCP pass function `ir_promote_i2i` (`ir_sccp.c:1920`) holds a raw
pointer (`insn`) into the instruction buffer `ctx->ir_base`. It then
calls `ir_const()` to create a new constant. If the constant pool is
full, `ir_const` -> `ir_const_ex` -> `ir_next_const` -> `ir_grow_bottom`
calls `realloc()`, which may move `ctx->ir_base` to a new address. The
held `insn` pointer is now dangling; the subsequent write through it
corrupts freed memory.

ASan stack trace:
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

### Why Our Change Exposed It
The old code reused a single function pointer (`DirectOrHostFn`, loaded
once at function entry) for all direct calls, requiring few constants per
function. The new code creates a unique `ir_CONST_ADDR(funcIdx *
sizeof(void*))` per call site. For large Wasm functions with many calls,
this exhausts the initial 4-entry constant pool (`IR_CONSTS_LIMIT_MIN`),
triggering the realloc during SCCP.

### Workaround (in ir_builder.cpp)
Pre-allocate a 256-entry constant pool instead of the minimum 4:
```cpp
ir_init(&Ctx, ir_flags, 256, IR_INSNS_LIMIT_MIN);
//                       ^^^ was IR_CONSTS_LIMIT_MIN (= 4)
```
This avoids the realloc during optimization.

### Proper Fix (upstream)
`ir_promote_i2i` (and likely other SCCP helpers) must reload their
`insn` pointer from `ctx->ir_base[ref]` after any call that may create
constants. Alternatively, `ir_grow_bottom` could use a growth strategy
that never moves the buffer (e.g., a linked-list of pages).

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
