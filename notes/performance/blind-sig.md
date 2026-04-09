# IR JIT Performance Analysis: `blind-sig`

**Date:** 2026-04-09
**Opt level:** O2
**Branch:** `ir_jit`

## Benchmark Results

| Kernel    | Mode   | Inst.Lat (µs) | WorkTime (µs) | TtV (µs)    |
|-----------|--------|----------------|---------------|-------------|
| blind-sig | IR_JIT | 205,548        | 47,493        | 334,107     |
| blind-sig | JIT    | 3,296,670      | 19,534        | 3,394,420   |
| blind-sig | AOT    | 327            | 19,134        | 21,384      |

**IR JIT runtime is 2.5x slower than AOT/LLVM JIT** (47ms vs 19ms).
Compilation latency (278ms for 546 functions) is expected for a JIT and not
investigated here. The investigation focuses on WorkTime.

---

## Module Profile

The blind-sig kernel implements RSA blind signatures using the `blind_rsa_signatures`
Rust crate, which depends on `num-bigint-dig` for arbitrary-precision integer
arithmetic. The wasm module has 548 defined functions (13 imports, 2 unreachable
trap stubs, 546 compiled).

Key functions in the hot path:

| Wasm Function | func[] | IR idx | IR Size | Role |
|---------------|--------|--------|---------|------|
| `monty_modpow` | 231 | 218 | 148 KB, 3612 lines | Montgomery modular exponentiation outer loop |
| `montgomery` | 230 | 217 | 38 KB, 1020 lines | Montgomery reduction; contains inner multiply loop |
| `mac3` | 219 | 206 | 451 KB, 11052 lines | Karatsuba bignum multiply (recursive) |
| `div_rem` | 238 | 225 | 209 KB | Bignum division with remainder |
| `extended_gcd` | 212 | 199 | 265 KB | Extended GCD for modular inverse |
| `__multi3` | 560 | 545 | 2 KB, 59 lines | 128-bit integer multiply (compiler-rt builtin) |

---

## Root Cause: `__multi3` Called Through Indirect Memory-Marshaled ABI

Same fundamental issue as ed25519 (see `notes/performance/ed25519.md`), but
exercised through a different call chain and at higher volume.

### The Hot Path

RSA blind signatures require modular exponentiation (`a^e mod n`), which is
implemented via Montgomery multiplication. The call chain is:

```
blind_sign (func[21])
  └─ modpow (func[198])
       └─ monty_modpow (func[231])
            ├─ montgomery (func[230]) — called 14 times per exponentiation step
            │    ├─ inner_loop_1 (O(n) iters) ── call __multi3 every iteration
            │    └─ inner_loop_2 (O(n) iters) ── call __multi3 every iteration
            ├─ div_rem (func[238]) — modular reduction
            └─ sub2 (func[243]) — conditional subtraction
```

For RSA-2048 with 32-bit wasm limbs, `n ≈ 64` limbs per bignum. The
Montgomery reduction has a nested loop structure: an outer loop over `n` limbs,
each containing an inner loop over `n` limbs that calls `__multi3`.

### Inner Loop: Montgomery Reduction (from wasm disassembly)

The tightest inner loop is at `035f6b` in the wasm binary, inside `montgomery`
(func[230]):

```wasm
loop                                      ;; iterate over modulus limbs
  local.get 6
  i32.const 16
  i32.add
  local.get 1
  i64.load 3 0                            ;; load modulus limb m[j]
  i64.const 0                             ;; hi1 = 0 (always)
  local.get 28                            ;; multiplier u
  i64.const 0                             ;; hi2 = 0 (always)
  call __multi3                           ;; 128-bit multiply: m[j] * u
  ;; ... load 128-bit result from stack frame ...
  ;; ... add result + carry to accumulator ...
  ;; ... store back to accumulator ...
  ;; ... compute new carry ...
  local.get 2
  i32.const -1
  i32.add
  local.tee 2
  br_if 0                                 ;; loop while limbs remain
end
```

### Inner Loop: IR Dump (from `/tmp/wasmedge_ir_217_after.ir`, BB46)

```
l_308 = LOOP_BEGIN(l_305, l_344);
d_309 = PHI(l_308, d_202, d_343);      // limb pointer (modulus)
d_310 = PHI(l_308, d_217, d_339);      // loop counter
d_311 = PHI(l_308, d_299, d_342);      // accumulator pointer
d_312 = PHI(l_308, c_10, d_338);       // carry (i64)

// Load modulus limb
d_315 = LOAD(l_308, d_314);            // m[j]

// --- Begin __multi3 call overhead ---
d_316 = LOAD(l_315, d_300);            // load func ptr from FuncTable
STORE(d_20, d_222);                    // marshal arg 0 (dest ptr)
STORE(d_304, d_315);                   // marshal arg 1 (m[j], i64)
STORE(d_303, 0);                       // marshal arg 2 (hi1 = 0)
STORE(d_301, d_298);                   // marshal arg 3 (multiplier u)
STORE(d_302, 0);                       // marshal arg 4 (hi2 = 0)
d_322 = PROTO(d_316, func(ptr,ptr):i64);
d_323 = CALL/2(d_322, d_2, d_20);     // indirect call
// --- End __multi3 call overhead ---

// Load 128-bit result
d_325 = LOAD(l_323, d_224);            // result_lo
d_334 = LOAD(l_331, d_227);            // result_hi

// Accumulate: acc[i] += result_lo + carry
d_328 = LOAD(l_325, d_327);            // load acc[i]
d_330 = ADD(ADD(d_325, d_328), d_312); // acc + result_lo + carry
STORE(l_328, d_327, d_330);            // store acc[i]

// Carry propagation
d_338 = ADD(d_337, d_333);             // new carry = result_hi + overflows

// Loop control
d_339 = ADD(d_310, 8);
l_340 = IF(l_334, d_339);              // branch: continue or exit
```

**Per iteration: 5 stores + 1 func-ptr load + indirect call + 5 loads in callee
\+ 2 result stores + return = 14 memory ops + indirect branch of pure ABI
overhead.**

### Call Volume Estimate

For RSA-2048 (64 limbs, 2048-bit exponent):

| Component | Iterations |
|-----------|-----------|
| Exponentiation steps (bit-scanning) | ~2048 |
| Montgomery multiplications per step (squaring + conditional mul) | ~1.5 avg |
| `montgomery` calls per multiplication (from wasm: 14 call sites) | variable |
| Inner loop iterations per `montgomery` call | ~64 |
| `__multi3` calls per inner iteration | 1 |

Conservative estimate: millions of `__multi3` calls per kernel execution.

### Per-Call Overhead Breakdown

| Cost Component | IR JIT | LLVM AOT (inlined) |
|---------------|--------|---------------------|
| Marshal 5 args to SharedCallArgs | 5 STOREs | 0 |
| Load function pointer from FuncTable | 1 LOAD | 0 |
| Indirect call + return | ~15 cycles | 0 |
| Unmarshal 5 args in callee | 5 LOADs | 0 |
| Load wasm memory base in callee | 1 LOAD | 0 (already in register) |
| Compute 128-bit multiply | 6 `imul` | 1 `mulq` (see below) |
| Store result to wasm memory | 2 STOREs | 2 STOREs (or 0 if further inlined) |
| Reload caller-saved registers after call | N spill/reloads | 0 |
| **Total extra overhead** | **~14 mem ops + ~5 wasted MULs + spills** | **0** |

---

## Secondary Issue: Wasted Multiplies in `__multi3`

The Montgomery loop calls `__multi3(dest, modulus_limb, 0, multiplier, 0)`.
Both high 64-bit halves (arg 2 and arg 4) are always zero. With inlining,
LLVM sees these constants and eliminates 4 of the 6 multiplies through constant
propagation:

```
// __multi3 IR (wasmedge_ir_545_after.ir):
d_15 = AND(d_10, 0xFFFFFFFF)   // lo_half(lo1)
d_16 = AND(d_6,  0xFFFFFFFF)   // lo_half(lo2)
d_17 = MUL(d_16, d_15)         // lo(lo1) * lo(lo2)     ← needed
d_18 = SHR(d_10, 32)           // hi_half(lo1)
d_19 = MUL(d_16, d_18)         // lo(lo2) * hi(lo1)     ← needed
d_20 = SHR(d_6, 32)            // hi_half(lo2)
d_21 = MUL(d_15, d_20)         // lo(lo1) * hi(lo2)     ← needed
d_28 = MUL(d_20, d_18)         // hi(lo1) * hi(lo2)     ← needed
d_38 = MUL(d_12, d_6)          // hi2 * lo1 = 0 * lo1   ← DEAD (hi2=0)
d_39 = MUL(d_10, d_8)          // lo1 * hi1 = lo1 * 0   ← DEAD (hi1=0)
```

Without inlining, the IR JIT cannot see that hi1 and hi2 are zero, so all 6
multiplies execute every iteration.

Furthermore, on x86-64, `mulq r64` produces a full 128-bit result in `rdx:rax`.
LLVM can replace the 4-multiply schoolbook decomposition with 1–2 native `mulq`
instructions. The IR backend emits 4 separate `imul` instructions since it has
no widening multiply operation.

---

## Additional Overhead: `mac3` (Karatsuba Multiplication)

For larger bignum multiplications (e.g., during key operations), the hot path
also flows through `mac3` (func[219], Karatsuba multiply). This function:

- Is **recursive** (calls itself for sub-problems)
- Has **130 wasm function calls** including recursive mac3 calls, BigInt
  arithmetic, and SmallVec allocations
- Compiles to 451 KB / 11,052 IR instructions with **1,218 STOREs and 1,122
  LOADs** — heavy spill traffic from register pressure across calls

The `mac3` function itself has zero `i64.mul` instructions; all multiplication
is delegated to called functions (recursive `mac3`, `__multi3`, etc.). Each of
these calls goes through the full memory-marshaled ABI.

With LLVM AOT, small callees are inlined, recursive calls can be partially
inlined (for the base case), and the register allocator sees the full picture.

---

## Why LLVM AOT Doesn't Have This Problem

LLVM compiles the entire wasm module as a single LLVM IR module:

1. **`__multi3` is inlined** — 117 bytes of wasm is well below LLVM's inline
   threshold. After inlining:
   - Zero memory marshaling; values stay in registers.
   - Constant propagation eliminates dead multiplies (hi=0).
   - LLVM may lower to hardware `mulq` for 64x64→128 multiply.

2. **Small helpers are inlined** — `sub2`, `add2`, SmallVec methods, etc. stay
   in registers across operations.

3. **Cross-function optimization** — LICM can hoist loop-invariant loads, CSE
   can eliminate redundant computations across call boundaries.

This explains why LLVM JIT (19.5ms) closely matches AOT (19.1ms) — both inline
`__multi3` and benefit from the same whole-module optimization.

---

## Proposed Fixes

Same recommendations as ed25519, prioritized by impact on this kernel:

### Fix 1: Inline `__multi3` at the IR Builder Level

When `visitCall` in `ir_builder.cpp` encounters a call to `__multi3` (detectable
by type signature `(i32, i64, i64, i64, i64) -> void`), emit the multiply
directly as IR instructions. This eliminates millions of indirect calls in the
Montgomery inner loop.

**Expected impact:** Largest single improvement. Eliminates ~14 memory ops +
indirect branch per inner-loop iteration. Estimated 30–50% of the 28ms gap.

### Fix 2: Constant Propagation Through Inlined `__multi3`

Once `__multi3` is inlined (Fix 1), the IR optimizer's SCCP pass will see that
hi1 and hi2 are zero at the Montgomery callsites and eliminate 2 of the 6
multiplies automatically.

### Fix 3: Register-Based Calling Convention for JIT-to-JIT Calls

Replace SharedCallArgs memory buffer with register-based argument passing for
intra-module calls. Benefits all 546 compiled functions, not just `__multi3`.

### Fix 4: x86-64 Widening Multiply

Add an IR opcode for `MUL_WIDE` (64x64→128) and pattern-match the schoolbook
decomposition. This would benefit all 128-bit multiply patterns, including the
ones in `mac3` and `div_rem`.

---

## Comparison with ed25519

| Aspect | blind-sig | ed25519 |
|--------|-----------|---------|
| Slowdown vs AOT | 2.5x (47ms vs 19ms) | 1.6x (1718ms vs 1046ms) |
| Root cause | `__multi3` call overhead | Same |
| Hot caller | `montgomery` (func[230]) | `__original_main` (func[8]) |
| `__multi3` call sites per hot function | 2 inner loops × O(n) | 809 direct |
| Total module functions | 548 | 17 |
| Bignum library | `num-bigint-dig` (Rust) | hand-tuned C (donna) |
| Algorithm | Montgomery modular exp (RSA) | Curve25519 scalar mul |

blind-sig has a higher relative slowdown (2.5x vs 1.6x) despite fewer direct
`__multi3` call sites per function, because the Montgomery reduction nests
`__multi3` inside a tight doubly-nested loop (O(n²) calls per reduction), and
the call frequency is amplified by the exponentiation loop.

---

## Appendix: IR Dump Reference

Dumps generated with `WASMEDGE_IR_JIT_DUMP=1` at O2.

| IR File | Wasm Function | Size | Notes |
|---------|---------------|------|-------|
| `wasmedge_ir_206_after.ir` | `mac3` (func[219]) | 451 KB, 11052 lines | Karatsuba multiply; 1218 STOREs, 1122 LOADs, 130 CALLs |
| `wasmedge_ir_199_after.ir` | `extended_gcd` (func[212]) | 265 KB | Extended GCD |
| `wasmedge_ir_225_after.ir` | `div_rem` (func[238]) | 209 KB | Bignum division |
| `wasmedge_ir_218_after.ir` | `monty_modpow` (func[231]) | 148 KB, 3612 lines | Exponentiation outer loop; 14 `montgomery` call sites |
| `wasmedge_ir_217_after.ir` | `montgomery` (func[230]) | 38 KB, 1020 lines | 12 CALLs, 6 LOOPs; inner loops call __multi3 |
| `wasmedge_ir_545_after.ir` | `__multi3` (func[560]) | 2 KB, 59 lines | 6 MULs, 6 LOADs, 2 STOREs; the hot callee |
| `wasmedge_ir_008_after.ir` | `blind_sign` (func[21]) | 151 KB | Entry point for the benchmark's measured work |
| `wasmedge_ir_085_after.ir` | `precompute` (func[98]) | 115 KB | RSA key precomputation |

### `__multi3` Callers (from wasm disassembly)

| Wasm Function | func[] | `__multi3` call sites | Context |
|---------------|--------|----------------------|---------|
| `montgomery` | 230 | 2 (in inner loops) | Hot: O(n²) calls per reduction |
| `mac3` | 219 | 3 | Karatsuba base case |
| `div_rem` | 238 | 3 | Bignum division |
| `u128_div_rem` | 556 | 4 | 128-bit division helper |

### IR Dump Index Mapping

The IR dump index is a running counter over compiled functions. With 13 imports
and 2 trap stubs (ci=397 at func[410], ci=422 at func[435]):

```
IR dump NNN → func[NNN + 13]           for NNN < 397
IR dump NNN → func[NNN + 13 + 1]       for 397 ≤ NNN < 421
IR dump NNN → func[NNN + 13 + 2]       for NNN ≥ 421
```
