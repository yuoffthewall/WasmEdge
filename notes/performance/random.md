# IR JIT Performance Analysis: `shootout-random`

**Date:** 2026-04-09
**Opt level:** O2
**Branch:** `ir_jit`

## Benchmark Results

| Kernel          | Mode   | Inst.Lat (us) | WorkTime (us) | TtV (us)   |
|-----------------|--------|---------------|---------------|------------|
| shootout-random | IR_JIT | 17,697        | 139,076       | 162,596    |
| shootout-random | JIT    | 207,069       | 87,547        | 298,544    |
| shootout-random | AOT    | 101           | 87,672        | 88,116     |

**IR JIT runtime is 59% slower than AOT/LLVM JIT** (139ms vs 88ms).
Compilation is 12x faster than LLVM JIT, so the investigation focuses on runtime.

---

## What the Benchmark Does

`shootout-random` is a linear congruential random number generator. The entire
hot path is in `__original_main` (func[9], IR dump 002), which runs this
computation **40 million times** (constant at wasm offset `0x1f5`):

```c
// Linear congruential PRNG
for (int i = 0; i < 40000000; i++)
    last = (last * 3877 + 29573) % 139968;
```

The wasm compiler unrolled the loop by 4, so the main loop body (`BB12` in IR)
executes ~10M iterations, each containing 4 `i32.rem_u` operations. A smaller
peel loop (`BB6` in IR) handles the `n % 4` remainder iterations.

The kernel is dominated by integer multiply and unsigned remainder. There is
no memory traffic, no function calls, no floating-point work in the hot loop.

---

## Root Cause: `i32.rem_u` Compiled as External Function Call

In `lib/vm/ir_builder.cpp:992-997`, the Wasm `i32.rem_u` instruction is
lowered to a **call to a C helper function** instead of an inline IR operation:

```cpp
case OpCode::I32__rem_u: {
    // IR MOD_U32 requires U32 operands; stack has I32. Call helper (same bits).
    ir_ref Proto = ir_proto_2(ctx, IR_FASTCALL_FUNC, IR_I32, IR_I32, IR_I32);
    ir_ref Func = ir_const_func_addr(ctx, (uintptr_t)&wasm_i32_rem_u, Proto);
    Result = ir_CALL_2(IR_I32, Func, Left, Right);
    break;
}
```

The helper (`ir_builder.cpp:36-38`):

```cpp
uint32_t wasm_i32_rem_u(uint32_t a, uint32_t b) {
    return b ? (a % b) : 0;
}
```

Meanwhile, the **signed** counterpart uses an inline IR operation:

```cpp
case OpCode::I32__rem_s:
    Result = ir_MOD_I32(Left, Right);  // compiles to inline `idiv`
    break;
```

The same pattern affects all four unsigned div/rem operations:

| Wasm opcode    | IR builder code           | Compiled as         |
|----------------|---------------------------|---------------------|
| `i32.div_s`    | `ir_DIV_I32(Left, Right)` | inline `idiv`       |
| **`i32.div_u`**| `ir_CALL_2(wasm_i32_div_u)` | **function call** |
| `i32.rem_s`    | `ir_MOD_I32(Left, Right)` | inline `idiv`       |
| **`i32.rem_u`**| `ir_CALL_2(wasm_i32_rem_u)` | **function call** |
| `i64.div_s`    | `ir_DIV_I64(Left, Right)` | inline `idiv`       |
| **`i64.div_u`**| `ir_CALL_2(wasm_i64_div_u)` | **function call** |
| `i64.rem_s`    | `ir_MOD_I64(Left, Right)` | inline `idiv`       |
| **`i64.rem_u`**| `ir_CALL_2(wasm_i64_rem_u)` | **function call** |

The original comment says "IR MOD_U32 requires U32 operands; stack has I32."
This is a type-system concern, not a hardware limitation. The dstogov/ir
framework provides `ir_BITCAST_U32()` to reinterpret I32 bits as U32 (no-op
at machine level), and `ir_MOD_U32()` / `ir_DIV_U32()` for inline unsigned
division.

### Call Volume

The benchmark executes **40,000,001 calls** to `wasm_i32_rem_u`:
- 10M iterations of the unrolled loop x 4 calls = 40M
- ~0-3 iterations of the peel loop x 1 call each
- 1 final call after the loop

---

## Generated Code Analysis

### Hot Loop (IR JIT, from `disas wasm_jit_002`)

The unrolled loop at offset +544 (BB12):

```asm
; --- rem_u iteration #1 ---
+544: imul   $0xf25,%eax,%eax          ; last * 3877
+550: lea    0x7385(%rax),%edi          ; + 29573 -> arg1
+556: mov    $0x222c0,%esi              ; 139968 -> arg2
+561: movabs $0x555555b71f92,%rax       ; load 64-bit helper address (10 bytes)
+571: call   *%rax                      ; CALL wasm_i32_rem_u

; --- rem_u iteration #2 ---
+573: imul   $0xf25,%eax,%eax
+579: lea    0x7385(%rax),%edi
+585: mov    $0x222c0,%esi
+590: movabs $0x555555b71f92,%rax
+600: call   *%rax

; --- rem_u iteration #3 ---
+602: imul   $0xf25,%eax,%eax
+608: lea    0x7385(%rax),%edi
+614: mov    $0x222c0,%esi
+619: movabs $0x555555b71f92,%rax
+629: call   *%rax

; --- rem_u iteration #4 ---
+631: imul   $0xf25,%eax,%eax
+637: lea    0x7385(%rax),%edi
+643: mov    $0x222c0,%esi
+648: movabs $0x555555b71f92,%rax
+658: call   *%rax

; --- loop control ---
+660: mov    0x48(%rsp),%ebp            ; load counter from stack (!)
+664: add    $0xfffffffc,%ebp           ; counter -= 4
+667: mov    %ebp,0x48(%rsp)            ; store counter back to stack
+671: jne    +544                       ; loop
```

### The Callee (Debug Build)

```asm
wasm_i32_rem_u:
    endbr64                             ; CET landing pad
    push   %rbp                         ; frame setup
    mov    %rsp,%rbp
    mov    %edi,-0x4(%rbp)              ; spill arg1 to stack
    mov    %esi,-0x8(%rbp)              ; spill arg2 to stack
    cmpl   $0x0,-0x8(%rbp)             ; if (b == 0)
    je     .Lzero                       ;   goto return 0
    mov    -0x4(%rbp),%eax              ; reload arg1
    mov    $0x0,%edx                    ; zero-extend for div
    divl   -0x8(%rbp)                   ; unsigned divide (memory operand)
    mov    %edx,%eax                    ; result = remainder
    jmp    .Lret
.Lzero:
    mov    $0x0,%eax
.Lret:
    pop    %rbp
    ret
```

*Note: This is a Debug build. A Release build would eliminate the frame
pointer and stack spills, but the call/ret overhead remains.*

### What Inline Code Would Look Like

With `ir_MOD_U32`, the IR backend would emit this per rem_u:

```asm
; inline unsigned remainder (IR_MOD_INT rule)
imul   $0xf25,%eax,%eax          ; last * 3877
lea    0x7385(%rax),%eax          ; + 29573
xor    %edx,%edx                  ; zero-extend
mov    $0x222c0,%ecx              ; 139968 (or kept in register across loop)
div    %ecx                       ; unsigned divide
mov    %edx,%eax                  ; result = remainder
```

5 instructions per rem_u, no call overhead, and the divisor can stay in a
register across loop iterations (the LSRA would allocate one).

---

## Per-Call Overhead Breakdown

| Cost component                  | Call-based (current)  | Inline (proposed)   |
|---------------------------------|-----------------------|---------------------|
| Load 64-bit function pointer    | `movabs` (10 bytes, 1 cycle) | eliminated   |
| Indirect call + return          | `call *%rax` + `ret` (~5 cycles) | eliminated |
| CET landing pad                 | `endbr64` (4 bytes)   | eliminated          |
| Frame setup/teardown (Debug)    | `push/pop rbp` + `mov` | eliminated        |
| Arg spill/reload (Debug)        | 4 memory ops          | eliminated          |
| Zero-check branch               | `cmp` + `je` (predictable) | eliminated     |
| Divisor load                    | from memory each time | register across loop |
| Loop counter spill              | to stack (call clobbers regs) | register   |
| **Core `div` instruction**      | ~25 cycles            | ~25 cycles          |

The `div` instruction itself dominates at ~25 cycles (Intel) / ~35 cycles
(AMD Zen) for 32-bit unsigned divide. The call overhead adds roughly
**8-15 cycles** per call on top (varying by microarchitecture and debug vs
release build). At 40M calls:

- **Call overhead:** 40M x ~10 cycles = ~400M cycles = **~130ms at 3GHz**
- **Actual gap:** 139ms - 88ms = **51ms**

The real savings will be less than the raw cycle count because much of the
call overhead overlaps with the `div` latency in the CPU pipeline. The `div`
instruction occupies the divider unit for ~25 cycles, during which some of the
call/ret microops can retire in parallel. Conservative estimate: **30-40ms
recovery**, closing the gap by ~60-80%.

### Why Not Full Parity with AOT?

Even with inline `div`, the IR JIT will likely remain slightly slower than AOT
because:

1. **No constant-divisor strength reduction.** LLVM replaces `% 139968` with
   a multiply-by-magic-number + shift sequence (~3 cycles) that avoids the
   slow `div` instruction entirely. The dstogov/ir backend does not perform
   this optimization for non-power-of-2 constants. It only optimizes
   power-of-2 modulo to `and` (the `IR_MOD_PWR2` rule in
   `thirdparty/ir/ir_emit_x86.h:3953-3963`).

2. **Loop counter lives on the stack.** Because the call clobbers all
   caller-saved registers, LSRA spills the loop counter. With inline code,
   the counter would stay in a register, but the div instruction itself
   clobbers `rax` and `rdx`, which constrains register allocation.

---

## Proposed Fix

### Immediate: Use `ir_BITCAST` + `ir_MOD_U32` / `ir_DIV_U32`

Replace the helper-function calls with inline IR operations using BITCAST to
satisfy the type system. BITCAST between I32 and U32 is a no-op (same width,
same bits, just a different IR type annotation):

```cpp
case OpCode::I32__rem_u: {
    ir_ref L = ir_BITCAST_U32(Left);
    ir_ref R = ir_BITCAST_U32(Right);
    Result = ir_BITCAST_I32(ir_MOD_U32(L, R));
    break;
}
case OpCode::I32__div_u: {
    ir_ref L = ir_BITCAST_U32(Left);
    ir_ref R = ir_BITCAST_U32(Right);
    Result = ir_BITCAST_I32(ir_DIV_U32(L, R));
    break;
}
case OpCode::I64__rem_u: {
    ir_ref L = ir_BITCAST_U64(Left);
    ir_ref R = ir_BITCAST_U64(Right);
    Result = ir_BITCAST_I64(ir_MOD_U64(L, R));
    break;
}
case OpCode::I64__div_u: {
    ir_ref L = ir_BITCAST_U64(Left);
    ir_ref R = ir_BITCAST_U64(Right);
    Result = ir_BITCAST_I64(ir_DIV_U64(L, R));
    break;
}
```

**Expected impact:** Eliminate 40M function calls. Close 60-80% of the 51ms
gap (~30-40ms improvement). Affects all kernels that use unsigned division,
not just shootout-random.

**Div-by-zero semantics:** The current helper returns 0 on division by zero.
Wasm spec requires a trap. The inline `div` instruction generates a hardware
`#DE` exception (SIGFPE). This is actually *more correct* than the current
behavior, but may need a signal handler if the runtime expects a clean trap.
For shootout-random, the divisor is always 139968 (constant), so this is not
a concern.

### Longer-Term: Constant-Divisor Strength Reduction in dstogov/ir

Extend the `IR_MOD_INT` / `IR_DIV_INT` matching rules in
`thirdparty/ir/ir_emit_x86.h` to replace non-power-of-2 constant divisors
with the multiply-by-reciprocal sequence. This is a standard compiler
optimization (libdivide algorithm, also used by GCC and LLVM). For
`% 139968`:

```asm
; multiply-by-magic for % 139968
mov    %eax,%ecx            ; save dividend
imul   $0x30C5ACFD,%eax     ; magic number (high 32 bits of reciprocal)
... shift sequence ...      ; extract remainder
```

This would replace the ~25-cycle `div` with a ~3-cycle `imul` + shifts,
bringing IR JIT to full parity with AOT/LLVM JIT for this kernel.

---

## IR Dump Reference

Dumps generated with `WASMEDGE_IR_JIT_DUMP=1` at O2.

### Key Constants in IR Dump 002 (`__original_main`)

| IR Constant | Value    | Meaning                              |
|-------------|----------|--------------------------------------|
| `c_12`      | 40000000 | Loop iteration count                 |
| `c_22`      | 3877     | LCG multiplier                       |
| `c_23`      | 29573    | LCG addend                           |
| `c_24`      | 139968   | LCG modulus                          |
| `c_25`      | func addr| Pointer to `wasm_i32_rem_u` helper   |
| `c_26`      | -4       | Unrolled loop stride                 |

### Hot Blocks

| Block | Role | Calls to `wasm_i32_rem_u` per iter |
|-------|------|------------------------------------|
| BB6   | Peel loop (`n % 4` iterations) | 1 |
| BB12  | Unrolled main loop (~10M iters) | 4 |
| BB15  | Post-loop (final random step)   | 1 |

### Files

| IR File | Wasm Function | Notes |
|---------|---------------|-------|
| `wasmedge_ir_002_after.ir` | `__original_main` (func[9]) | Hot function, 174 lines, contains both loops |
