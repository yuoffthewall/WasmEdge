# Debugging Summary: Quicksort SEGV in IR_JIT Mode

## The Problem
The Sightglass `quicksort` benchmark failed consistently under the `IR_JIT` backend, causing a crash due to an AddressSanitizer (ASan) SEGV fault. The crash reported an invalid memory access (`READ`) at an out-of-bounds address (e.g. `0x7b5659c05c7c`), effectively pointing approximately ~4GB out of bounds from the native Wasm memory boundary.

## The Root Cause
The root cause was a conflict between WebAssembly's 32-bit memory semantics and an aggressive induction variable optimization pass within the underlying `dstogov/ir` JIT engine.

1. **Wasm 32-bit Linear Memory Wrapping:** Wasm memory addresses are strictly computed as 32-bit integers. If a pointer traversing an array (like in `quicksort`) drops below zero, Wasm relies on 32-bit integer wrapping. For example, `base - 4` evaluates to `0xFFFFFFFC`. In a correct IR generation phase, this 32-bit value will eventually be zero-extended (`ir_ZEXT_A`) to a 64-bit native pointer size before being added to `MemoryBase`.
2. **Aggressive Induction Variable Promotion:** During the JIT compiler's Sparse Conditional Constant Propagation (SCCP) phase, the optimization pass `ir_try_promote_induction_var_ext` noticed that a loop variable (our 32-bit Wasm memory index) was being zero-extended on each iteration. To "optimize" this, the compiler *hoisted* the zero-extension out of the loop and promoted the induction variable up to a 64-bit integer.
3. **Broken Wrapping Semantics:** Because the induction variable arithmetic was now promoted and operating in a 64-bit domain *before* the math finished, Wasm's intrinsic 32-bit modulus semantics broke. Adding a negative offset such as `-4` (which represents the unsigned 32-bit constant `0xFFFFFFFC`) simply added `4,294,967,292` to a 64-bit base offset without wrapping cleanly at 32 bits. 
4. **The Crash:** The compiled native code would generate an instruction like `LOAD(MemoryBase + 4294967292)`, blasting far outside the Wasm memory allocation and causing an immediate hardware SEGV trap.

## The Fix
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


Here's why the code zero-extends a 32-bit value “every iteration” and why that’s intentional.

---

## Why the code zero-extends at all

WebAssembly linear memory is defined in terms of **32-bit** indices. On a 64-bit host, the real pointer is:

`effective_address = MemoryBase + offset`

`MemoryBase` is 64-bit; the Wasm “offset” is 32-bit. So the 32-bit value must be **zero-extended to 64-bit** at the point where you form the pointer. That’s a requirement of the ABI / memory model, not a choice.

In the Wasm→IR lowering this is done in one place: **`buildMemoryAddress`** in `ir_builder.cpp`:

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
- Only when building the actual pointer do we do **ZEXT_A** (32-bit → address width) then **ADD_A** with `MemoryBase`.

So the “why” for the zero-extension is: **to form a 64-bit pointer from a 32-bit Wasm index while keeping all index math in 32-bit.**

---

## Why it happens “every iteration”

The IR builder does **not** keep a single 64-bit “memory pointer” in the loop. It keeps the Wasm index as **32-bit** (on the stack / in phis / in locals). Every **load and store** calls `buildMemoryAddress` with whatever 32-bit value is on the stack at that moment:

```1559:1562:lib/vm/ir_builder.cpp
  if (IsLoad) {
    // Load operations: pop address, load from memory, push result
    ir_ref BaseAddr = pop(); // Address from stack (i32)
    ir_ref EffectiveAddr = buildMemoryAddress(BaseAddr, Offset);
```

So the pattern in the generated IR is:

- Loop has a **phi** (or chain of values) that represents the **32-bit** index.
- Each load/store in the loop body uses that index → calls `buildMemoryAddress` → so each use gets **ZEXT_A(that 32-bit value)** and then `MemoryBase + result`.

So the zero-extension appears **at every load/store**, i.e. “every iteration” (or more precisely, every memory operation in the loop). That’s not an extra optimization; it’s the direct result of:

1. Representing addresses as 32-bit until the last moment.
2. Building the real pointer only at each load/store via `buildMemoryAddress`.

So: **the code does the zero-extension every iteration because each iteration performs memory operations, and each of those operations builds its pointer from the current 32-bit index via `buildMemoryAddress`, which always does the ZEXT at that use site.** The design intentionally keeps all index arithmetic in 32-bit and only extends to 64-bit when forming the pointer for a load/store.