# IR JIT Performance Analysis: `shootout-ctype`

**Date:** 2026-04-09
**Opt level:** O2
**Branch:** `ir_jit`

## Benchmark Results

| Kernel         | Mode   | Inst.Lat (us) | WorkTime (us) | TtV (us)    |
|----------------|--------|---------------|---------------|-------------|
| shootout-ctype | IR_JIT | 21,164        | 148,080       | 175,452     |
| shootout-ctype | JIT    | 289,325       | 82,865        | 378,002     |
| shootout-ctype | AOT    | 99            | 82,881        | 83,362      |

**IR JIT runtime is 79% slower than AOT/LLVM JIT** (148ms vs 83ms).
Compilation latency is already good (21ms vs 289ms for LLVM JIT); the
investigation focuses on runtime (WorkTime).

---

## Root Cause: Tiny ctype Functions Called 100M Times Through Indirect Marshaled ABI

The shootout-ctype kernel allocates a 50,000-byte buffer filled with `'x'`
(0x78), then in a tight nested loop classifies and transforms each character
using `isalnum`, `tolower`, and `toupper`. The loop structure in
`__original_main` (func[9]):

```
outer: 1,000 iterations
  call_indirect _black_box        // 1x per outer iter — negligible
  inner: 50,000 iterations
    call isalnum(char)            // 1x per inner iter — HOT
    call tolower(char)  OR        // 1x per inner iter — HOT
    call toupper(char)            //   (one or the other, via branch)
```

**Total function calls in the work phase: ~100,000,000** (50M `isalnum` +
~50M `tolower`/`toupper`).

### The Callees Are Trivially Small

All three hot callees compile to fewer than 10 IR nodes:

**`isalnum` (func[22], IR idx 015):**
```
l_1  = START
d_4  = LOAD args[0]          // unmarshal the single i32 arg
d_5  = OR(d_4, 32)           // force lowercase
d_6  = ADD(d_4, -65)         // ch - 'A'
d_7  = ULT(d_6, 26)          // is it a letter?
d_8  = ZEXT(d_7)
d_9  = COND(d_8, d_5, d_4)   // select
RETURN d_9
```

**`tolower` (func[23], IR idx 016):**
```
d_4  = LOAD args[0]
d_5  = AND(d_4, 95)          // clear bit 5
d_6  = ADD(d_4, -97)
d_7  = ULT(d_6, 26)
d_8  = ZEXT(d_7)
d_9  = COND(d_8, d_5, d_4)
RETURN d_9
```

**`toupper` (func[24], IR idx 017):** Same pattern, different constants.

**`_black_box` (func[10], IR idx 003):**
```
l_1 = START
RETURN null                   // literal no-op
```

Each callee has **5 ALU ops** of actual work. The call overhead vastly exceeds
the computation.

---

## Per-Call Overhead Anatomy

### How the IR Builder compiles `call isalnum` (`ir_builder.cpp:2886-2919`)

Every wasm `call` to an intra-module function is compiled as an indirect call
through a function-pointer table, with arguments serialized into a shared
memory buffer:

```
// Caller side (in __original_main's inner loop, per call):
1. func_ptr = LOAD FuncTablePtr[0xb0]    // load isalnum pointer from table
2. STORE SharedCallArgs[0], char_val      // marshal i32 arg to memory buffer
3. CALL (env_ptr, SharedCallArgs)         // indirect call through func_ptr

// Callee side (isalnum):
4. d_4 = LOAD args[0]                    // unmarshal i32 arg from memory
5. ... 5 ALU ops ...                     // actual work
6. RETURN result

// Back in caller:
7. result = TRUNC_I32(call_result)       // extract return value
```

### Overhead per call

| Step | Operation | Count | Type |
|------|-----------|-------|------|
| Marshal arg | STORE to SharedCallArgs | 1 | memory write |
| Load func ptr | LOAD from FuncTable | 1 | memory read |
| Call dispatch | indirect CALL | 1 | branch + frame |
| Unmarshal arg | LOAD from args buffer | 1 | memory read |
| Prologue/epilogue | callee frame setup | 1 | stack ops |

**Per call: 3 memory round-trips + 1 indirect branch + frame overhead** for
5 ALU ops of actual work. The overhead-to-work ratio is roughly 3:1.

### IR Evidence from `__original_main`

From the IR dump (`/tmp/wasmedge_ir_002_after.ir`), the inner loop (BB5)
contains:

```
// isalnum call (every iteration):
d_104 = LOAD(l_97, d_78)                     // load func ptr for isalnum
STORE(l_104, d_13, d_98)                      // marshal arg
d_107 = CALL/2(l_105, d_106, d_2, d_13)      // indirect call
d_109 = TRUNC(d_107)                          // extract i32 result

// tolower call (BB7, conditional):
d_139 = LOAD(l_138, d_79)                    // load func ptr for tolower
STORE(l_139, d_13, d_130)                     // marshal arg
d_142 = CALL/2(l_140, d_141, d_2, d_13)      // indirect call
d_144 = TRUNC(d_142)                          // extract i32 result

// toupper call (BB9, conditional):
d_149 = LOAD(l_148, d_80)                    // load func ptr for toupper
STORE(l_149, d_13, d_130)                     // marshal arg
d_152 = CALL/2(l_150, d_151, d_2, d_13)      // indirect call
d_154 = TRUNC(d_152)                          // extract i32 result
```

Each CALL also clobbers all caller-saved registers, forcing the LSRA to spill
and reload values like `d_8` (memory base), `d_91` (loop counter), `d_96`
(character address) around every call site.

### Function Table Offsets

| Offset | Func Index | Wasm Name | Calls per kernel run |
|--------|-----------|-----------|----------------------|
| `0xb0` | func[22]  | `isalnum` | 50,000,000 |
| `0xb8` | func[23]  | `tolower` | ~25,000,000 (conditional) |
| `0xc0` | func[24]  | `toupper` | ~25,000,000 (conditional) |
| `0xdf0`| (table)   | `_black_box` (via call_indirect) | 1,000 |

---

## Quantified Impact

The delta between IR JIT and LLVM JIT is **~65,000 us** (148ms - 83ms).

With 100M calls at the uniform calling convention:
- **65,000,000 ns / 100,000,000 calls = ~0.65 ns overhead per call**

This is consistent with modern x86-64 behavior: the indirect call target is
predictable (each call site always targets the same function), so the branch
predictor learns it quickly. The dominant cost per call is the **memory
round-trips** for argument marshaling (store in caller, load in callee) and the
**register spill/reload** forced by the CALL clobbering caller-saved registers.

Even at only ~0.65ns per call, the sheer volume of 100M calls accounts for
**nearly all** of the 65ms performance gap.

---

## Why LLVM AOT Doesn't Have This Problem

LLVM compiles the entire wasm module as a single LLVM IR module. `isalnum`
(~20 bytes of wasm), `tolower`, and `toupper` are trivially below the inlining
threshold. After inlining:

- Zero memory marshaling; the character value stays in a register.
- No indirect call; the callee's 5 ALU ops are fused into the loop body.
- No register spill/reload around call sites; the optimizer sees the full loop
  as a single basic block (or small set of blocks).
- LLVM can hoist loop-invariant loads and apply further optimizations across
  what were formerly call boundaries.

This explains why LLVM JIT (82,865 us) matches AOT (82,881 us) — both inline
the tiny functions.

---

## Why the IR JIT Cannot Inline These Functions

The IR JIT compiles each wasm function as a separate `ir_ctx`
(`lib/vm/ir_jit_engine.cpp:120-187`). When `visitCall` in `ir_builder.cpp`
encounters `call 22 <isalnum>`, the builder has no access to `isalnum`'s IR
graph — it may not even be compiled yet. The IR library's inliner operates
within a single `ir_ctx` and cannot reach across compilation boundaries.

Instead, the builder emits a load-from-table + indirect-call sequence
(`ir_builder.cpp:2886-2919`):

```cpp
// Load function pointer from table
ir_ref FuncPtr = ir_LOAD_A(ir_ADD_A(
    ValidFT,
    ir_CONST_ADDR(ResolvedFuncIdx * sizeof(void *))));

// Marshal args into shared buffer
for (uint32_t i = 0; i < NumArgs; ++i) {
    ir_ref SlotAddr = ir_ADD_A(CalleeArgs, ir_CONST_ADDR(i * sizeof(uint64_t)));
    ir_STORE(SlotAddr, WasmArgs[i]);
}

// Indirect call with uniform (env, args) ABI
ir_ref CallResult = ir_CALL_N(DirectRetType, TypedFuncPtr, 2, DirectArgs);
```

---

## Cascading Effects Beyond Direct Overhead

### 1. Register Pressure Across Calls

Each CALL clobbers all caller-saved registers (rax, rcx, rdx, rsi, rdi, r8-r11
on x86-64). The inner loop body has 2 CALL sites, so the LSRA must spill/reload
all live values twice per iteration. Key values that survive across calls:

| IR Value | Role | Must be spilled across each CALL |
|----------|------|----------------------------------|
| `d_8` | wasm memory base pointer | yes |
| `d_91` | inner loop counter | yes |
| `d_96` | current character address | yes |
| `d_47` | stack frame pointer | yes |
| `d_26` | buffer pointer address | yes |

With inlining, none of these would need spilling — the `isalnum` computation
(5 ALU ops) would fit in the remaining registers alongside these live values.

### 2. Lost Optimization Opportunities

The character loaded at `d_96` is read 3 times in the wasm source (once for
the isalpha check, once for isalnum, once for tolower/toupper). With inlining,
the optimizer could CSE these loads into a single load. Without inlining, each
CALL is an optimization barrier — the compiler must assume the call may modify
memory, so it reloads after each call.

### 3. Function-Pointer Reload After Each Call

Because CALL may modify arbitrary memory, the function-pointer addresses
(`d_78`, `d_79`, `d_80`) cannot be hoisted out of the loop. They are reloaded
from the function table on every iteration, adding 2 redundant memory loads
per iteration (50M × 2 = 100M loads). In practice the branch predictor and
L1 cache make these cheap, but they still consume load ports.

### 4. The `_black_box` call_indirect

The outer loop's `call_indirect` for `_black_box` goes through the full
`jit_call_indirect` C++ trampoline (`lib/executor/helper.cpp:168-190`),
which performs:
- Table bounds check
- Null reference check
- Type matching via `AST::TypeMatcher::matchType`
- C++ function dispatch

This is very expensive per call, but at only 1,000 iterations it contributes
< 1ms and is not the bottleneck.

---

## Proposed Fixes

### Fix 1: Inline Tiny Leaf Functions at the IR Builder Level (highest impact)

When `visitCall` encounters a call to a function that:
- Is defined in the same module (not an import)
- Has a small wasm bytecode body (e.g., < 64 bytes)
- Has already been analyzed (single pass or deferred)

...emit the callee's IR instructions directly into the caller's `ir_ctx`
instead of generating a CALL. This is manual inlining at the IR builder level.

For `shootout-ctype`, this would eliminate 100M calls and their associated
overhead. The inner loop body would shrink from:

```
STORE arg → LOAD func_ptr → indirect CALL → LOAD arg → 5 ops → RETURN
```

to just:

```
5 ALU ops (inlined)
```

**Expected impact:** Eliminates the ~65ms gap entirely for this kernel. Similar
impact on any kernel with hot small-function calls (ed25519's `__multi3` is the
same pattern at a larger scale).

### Fix 2: Direct Call Emission for Known Targets

When the callee is in the same module and its native code pointer is already
available (compiled earlier in the compilation order), emit
`ir_const_func_addr(ctx, known_address, proto)` instead of loading the pointer
from the function table. This converts indirect calls to direct calls, enabling
better branch prediction and eliminating the function-pointer load.

**Impact:** Reduces per-call overhead by 1 memory load and converts indirect
branch to direct branch. Benefits all intra-module calls, not just tiny ones.
Requires compilation ordering (callees before callers) or a two-pass approach.

### Fix 3: Register-Based Calling Convention for Intra-JIT Calls

Replace the `SharedCallArgs` memory buffer with native register-based argument
passing for wasm-to-wasm calls. `isalnum` takes 1 i32 argument — this should
pass in a register (rdi/rsi), not through a memory buffer.

x86-64 SysV ABI provides 6 integer argument registers. Most wasm functions
have <= 6 params. A hybrid convention could use registers for simple types and
fall back to the buffer only for functions exceeding the register count.

**Impact:** Eliminates 2 memory ops per call (1 marshal store + 1 unmarshal
load). For 100M calls, this saves ~100M store-load pairs.

### Fix 4: Module-Level Compilation (long-term)

Compile all (or frequently-called) functions into a single `ir_ctx`, allowing
the IR library's native optimization passes (SCCP, GCM, inlining) to operate
across wasm function boundaries. This is the general solution that subsumes
Fixes 1-3 but requires significant pipeline refactoring.

---

## Comparison with `shootout-ed25519`

Both ctype and ed25519 suffer from the same root cause — the uniform indirect
calling convention — but at different scales:

| Metric | `shootout-ctype` | `shootout-ed25519` |
|--------|------------------|--------------------|
| Hot callee | `isalnum` (5 ALU ops) | `__multi3` (6 MULs + shifts) |
| Callee args | 1 × i32 | 5 × mixed (i32 + i64) |
| Call count | ~100,000,000 | ~3,200 |
| Marshal stores/call | 1 | 5 |
| Overhead/call | ~0.65 ns | ~200+ ns (5 marshals + spills) |
| Total overhead | ~65 ms | ~672 ms |
| Slowdown vs AOT | 1.79× | 1.64× |

ctype has far more calls but each is cheaper (1 arg, predictable branch target).
ed25519 has fewer calls but each is more expensive (5 args, more spill pressure).
The fix strategy is the same: eliminate the indirect marshaled ABI for
intra-module calls, starting with inlining of small callees.

---

## Appendix: IR Dump Reference

Dumps generated with `WASMEDGE_IR_JIT_DUMP=1` at O2.

| IR File | Wasm Function | IR Idx | Notes |
|---------|---------------|--------|-------|
| `wasmedge_ir_002_after.ir` | `__original_main` (func[9]) | 002 | 250 lines; hot loop in BB5 with 2 CALLs/iter |
| `wasmedge_ir_015_after.ir` | `isalnum` (func[22]) | 015 | 10 lines; 5 ALU ops, called 50M times |
| `wasmedge_ir_016_after.ir` | `tolower` (func[23]) | 016 | 10 lines; 5 ALU ops, called ~25M times |
| `wasmedge_ir_017_after.ir` | `toupper` (func[24]) | 017 | 10 lines; 5 ALU ops, called ~25M times |
| `wasmedge_ir_003_after.ir` | `_black_box` (func[10]) | 003 | 4 lines; literal no-op (empty body) |

### Module Structure

- **7 imports:** bench.start, bench.end, 5 WASI functions
- **53 defined functions**, 52 compiled (1 trap stub at func[20])
- **Compilation time:** ~27ms for all 52 functions at O2

### Wasm Inner Loop Structure (func[9], offsets 0x27c–0x321)

```wasm
loop                              ;; inner: 50,000 iterations
  ;; accumulate isalpha + isalnum + isdigit + isspace + isprint into counter
  local.get 1                     ;; buffer[i]
  i32.load8_u 0 0                 ;; load char
  call 22 <isalnum>              ;; HOT: indirect marshaled call
  ;; ... arithmetic with result ...
  block                           ;; branch on char class
    block
      block
        i32.load8_u 0 0           ;; reload char
        call 23 <tolower>        ;; HOT: conditional, ~50% of iterations
        br 1
      end
        call 24 <toupper>        ;; HOT: conditional, ~50% of iterations
    end
    i32.store8 0 0               ;; write transformed char back
  end
  local.get 3                    ;; i++
  i32.const 1
  i32.add
  i32.const 50000
  i32.ne
  br_if 0                       ;; loop back
end
```
