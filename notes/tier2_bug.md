# Tier-2 Bugs (ir backend & WasmEdge)

Bugs discovered during tier-2 LLVM recompilation pipeline development.
Bugs in `thirdparty/ir` are logged but fixed in-place (not deferred) per user request.

---

## Bug 1: `ir_emit_llvm` emits `zext i32 to ptr` instead of `inttoptr` — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `thirdparty/ir/ir_emit_llvm.c`, `IR_ZEXT` / `IR_SEXT` / `IR_TRUNC` cases (~line 1149)
**Category:** ir backend bug

**Root cause:** The `IR_ZEXT` handler unconditionally emitted `zext` regardless
of whether the target type is a pointer. In the dstogov/ir type system,
`IR_ADDR` (pointer) satisfies `IR_IS_TYPE_INT()` because it's defined as
`uintptr_t` — so the integer-only guards don't catch it. LLVM IR's `zext` only
works between integer types; `i32 → ptr` requires `inttoptr`.

**Reproduction:**
```
WASMEDGE_SIGHTGLASS_KERNEL=quicksort WASMEDGE_TIER2_ENABLE=1 \
WASMEDGE_TIER2_THRESHOLD=10 WASMEDGE_TIER2_DUMP_IR=1 \
WASMEDGE_SIGHTGLASS_MODE=IR_JIT WASMEDGE_IR_JIT_OPT_LEVEL=2 \
./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```
Check `/tmp/tier2_wasm_tier2_*.ll` for the pattern `zext i32 ... to ptr`.

**Example (from `wasm_tier2_019.ll:39`):**
```llvm
%d32 = sub i32 %d29, 4
%d33 = zext i32 %d32 to ptr       ; <-- INVALID
```
**Expected:** `%d33 = inttoptr i32 %d32 to ptr`

**Fix:** In the `IR_ZEXT`, `IR_SEXT`, and `IR_TRUNC` cases, added checks:
- If target is `IR_ADDR` and source is not `IR_ADDR` → emit `inttoptr`
- If source is `IR_ADDR` and target is not `IR_ADDR` → emit `ptrtoint`
- Otherwise → emit original instruction (`zext`/`sext`/`trunc`)

**Impact:** All tier-2 compilations for wasm functions using memory (nearly all)
would fail to parse. This was the first and most common error.

---

## Bug 2: Empty blocks skipped but still referenced by PHI nodes — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `thirdparty/ir/ir_emit_llvm.c`, block iteration loop (~line 1014),
  `ir_emit_phi()` (~line 363), `ir_emit_if()` (~line 647), END/LOOP_END (~line 1253),
  `ir_emit_switch()` (~line 700, 732)
**Category:** ir backend bug

**Root cause:** Two mechanisms in `ir_emit_llvm` are inconsistent:

1. **Block emission** (line 1014): Blocks with `IR_BB_EMPTY` flag are skipped
   entirely — no label, no instructions.
2. **Branch emission**: `ir_skip_empty_target_blocks()` is called to redirect
   branches past empty blocks. E.g., `br i1 %cond, label %l15, label %l14`
   instead of `br i1 %cond, label %l13, label %l14` (where l13 is empty).
3. **PHI emission** (line 363): PHI nodes use raw predecessor block numbers
   from `ctx->cfg_edges`, which still reference the empty block.

Result: PHI at `%l15` says `phi i32 [%d51, %l13]` but block `l13` doesn't exist
in the output, and no branch targets `l13`. LLVM verifier rejects this with
"PHINode should have one entry for each predecessor of its parent basic block".

**Fix:** Three coordinated changes:
1. **Emit all empty blocks** as trivial jumps: `l13: br label %l15`
2. **Stop using `ir_skip_empty_target_blocks()`** in the LLVM emitter. Branches
   now target the original block (including empty ones).
   - Added `ir_get_true_false_blocks_noskip()` — identical to
     `ir_get_true_false_blocks()` but without the skip call.
   - Removed the skip call in switch default/case emission (lines 700, 732).
   - Removed the skip call in END/LOOP_END emission (line 1253).
3. **PHI emission unchanged** — raw predecessors are now correct because the
   empty blocks exist in the output.

LLVM's optimization passes trivially eliminate the extra jumps during tier-2
compilation, so there is no performance cost.

---

## Bug 3: Malformed `select` for non-boolean conditions — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `thirdparty/ir/ir_emit_llvm.c`, `ir_emit_conditional_op()` (~line 567)
**Category:** ir backend bug

**Root cause:** `ir_emit_conditional_op()` unconditionally called
`ir_emit_def_ref(ctx, f, def)` first (which prints `\t%d{N} = `), then for
non-boolean conditions branched into code that emits a comparison instruction
*on a new line* (`\t%t{N} = icmp ne ...\n`), followed by the select.

This produced:
```
\t%d404 = \t%t404 = icmp ne i32 %d396, 0
select i1 %t404, i32 -1, i32 %d403
```
Two assignment operators on one line, then `select` on the next with no
assignment. Both are invalid LLVM IR.

**Expected:**
```
\t%t404 = icmp ne i32 %d396, 0
\t%d404 = select i1 %t404, i32 -1, i32 %d403
```

**Fix:** For non-boolean conditions (integer and float), moved
`ir_emit_def_ref()` to AFTER the comparison instruction. The comparison is
emitted first as a standalone instruction, then the select with its own
assignment.

---

## Bug 4: `llvm.cttz`/`llvm.ctlz` second arg missing `i1` type — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `thirdparty/ir/ir_emit_llvm.c`, `ir_emit_bitop()` (~line 527)
**Category:** ir backend bug

**Root cause:** `ir_emit_bitop()` emitted the "is_zero_poison" parameter as
bare `0` instead of `i1 0`. LLVM intrinsic `@llvm.cttz.i32(i32, i1)` requires
a typed second argument. Without the type, the LLVM parser sees `0` at argument
position and expects a type annotation.

**Example:**
```llvm
%d300 = call i32 @llvm.cttz.i32(i32 %d198, 0)    ; INVALID
%d300 = call i32 @llvm.cttz.i32(i32 %d198, i1 0)  ; CORRECT
```

**Fix:** Changed `fprintf(f, ", 0")` to `fprintf(f, ", i1 0")` in the `poison`
branch of `ir_emit_bitop()`.

**Impact:** 30 functions across multiple kernels (blind-sig, bz2, etc.) failed
tier-2 parse. This was the second most common error after Bug 1.

---

## Bug 5: `bitcast i1 to i8` — invalid cast between different-width types — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `thirdparty/ir/ir_emit_llvm.c`, `IR_BITCAST` handler (~line 1224)
**Category:** ir backend bug

**Root cause:** The `IR_BITCAST` handler only checked for `IR_ADDR` conversions
(inttoptr/ptrtoint). For all other cases, it emitted `bitcast`. But LLVM's
`bitcast` requires source and destination to have the same bit width. `i1` (1
bit) to `i8` (8 bits) is not valid for bitcast.

In the dstogov/ir type system, `IR_BOOL` has `ir_type_size == 1` (byte) and
`IR_I8`/`IR_U8` also have `ir_type_size == 1` (byte). So a simple size check
`ir_type_size[src] != ir_type_size[dst]` does NOT catch this case — they appear
same-sized despite being `i1` vs `i8` in LLVM.

**Example (from `wasm_tier2_349.ll:3512`):**
```llvm
%d2880 = bitcast i1 %d2879 to i8   ; INVALID
%d2880 = zext i1 %d2879 to i8      ; CORRECT
```

**Fix:** Extended the `IR_BITCAST` handler with a new branch:
```c
else if (IR_IS_TYPE_INT(insn->type) && IR_IS_TYPE_INT(src_type)
         && (ir_type_size[insn->type] != ir_type_size[src_type]
             || insn->type == IR_BOOL || src_type == IR_BOOL))
```
- If target is `IR_BOOL` → emit `trunc`
- If source is `IR_BOOL` → emit `zext`
- If target wider → `zext`; narrower → `trunc`

---

## Bug 6: Float constants and special values not round-tripping — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `thirdparty/ir/ir_emit_llvm.c`, float constant emission (~line 291–323)
**Category:** ir backend bug (two sub-issues)

### Bug 6a: Float constant precision loss

**Root cause:** Float constants (`IR_FLOAT`) were emitted using C `%e` format
(6 significant digits). LLVM IR requires that float constants round-trip exactly
when parsed as double and truncated to float. `%e` doesn't guarantee this —
`1.700000e+38` may not convert back to the same 32-bit float value.

**Fix:** All float constants (except `0.0` and `NaN`) now use hex-double
format: `0x%016PRIX64`. The float is promoted to double, then the double's raw
IEEE 754 bits are printed in hex. This is the canonical LLVM approach.

### Bug 6b: Double infinity/NaN emitted as text

**Root cause:** `fprintf(f, "%e", d)` on `±inf` produces the text `inf` or
`-inf`, which LLVM IR doesn't accept as constant syntax. Similarly, `nan` was
emitted via `fprintf(f, "nan")` which is also invalid.

**Example:**
```llvm
%d1592 = fcmp olt double %d1584, inf   ; INVALID
%d1592 = fcmp olt double %d1584, 0x7FF0000000000000  ; CORRECT
```

**Fix:** Combined `isnan(d)` and `isinf(d)` into a single check that emits hex
format for all special FP values. Also fixed `log10(d)` → `log10(fabs(d))` to
avoid NaN when `d` is negative.

---

## Bug 7: Function definition missing `x86_fastcallcc` — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `lib/vm/ir_builder.cpp`, `initialize()` (~line 293)
**Category:** WasmEdge bug (our code)

**Root cause:** The ir_ctx was initialized with:
```c
uint32_t ir_flags = IR_FUNCTION;
```
This omits the calling convention flag. `ir_emit_llvm()` reads
`ctx->flags & IR_CALL_CONV_MASK` to emit the function definition's calling
convention. Without `IR_CC_FASTCALL` (0x02), the function definition gets
no calling convention annotation — defaulting to the C calling convention.

Meanwhile, tier-1 callers (generated by `ir_jit_compile`) use `x86_fastcallcc`
because all prototypes are created with `IR_FASTCALL_FUNC`. On x86-64 Linux:
- **C convention:** args in `rdi`, `rsi`, `rdx`, `rcx`
- **fastcall:** first two integer args in `ecx`, `edx`

When tier-2 code (C convention) replaced a tier-1 function (called with fastcall),
the function read `env` from `rdi` instead of `ecx`, getting garbage — segfault.

**Fix:** Changed to `IR_FUNCTION | IR_FASTCALL_FUNC`.

**Impact:** Caused segfaults in any kernel that executed tier-2 compiled code.
Affected `shootout-ackermann` and `rust-compression` (though rust-compression
has additional issues — see Bug 11).

---

## Bug 8: LLVM native target not initialized — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `lib/vm/tier2_compiler.cpp`, `Tier2Compiler` constructor (~line 36)
**Category:** WasmEdge bug (our code)

**Root cause:** `LLVMOrcCreateLLJIT()` requires LLVM's native target to be
registered. Without calling `LLVMInitializeNativeTarget()`,
`LLVMInitializeNativeAsmPrinter()`, and `LLVMInitializeNativeAsmParser()`, ORC
LLJIT fails with `"Unable to find target for this triple (no targets are registered)"`.

**Fix:** Added all three initialization calls to `Tier2Compiler()` constructor.

---

## Bugs 9, 10, 11: ir_print_const, ir_get_str, NIY instruction — FIXED (obsolete)

**Date:** 2026-04-03
**Status:** Fixed (root cause was shared — see below)
**Category:** WasmEdge bug (our pipeline design)

**Original symptoms:**
- Bug 9: `ir_print_const` assertion with IR_FUNC nodes (blake3-scalar)
- Bug 10: `ir_get_str` strtab not preserved after compilation (shootout-nestedloop)
- Bug 11: NIY instruction assertion in ir_emit_llvm (various)

**Root cause:** All three were symptoms of the same problem — the tier-2
pipeline was calling `ir_emit_llvm()` on a **post-`ir_jit_compile()` ir_ctx**.
`ir_jit_compile()` destructively mutates the context (optimization passes,
register allocation, codegen), leaving it in an unusable state: strtab freed,
instruction opcodes rewritten, data structures corrupted.

**Fix:** Refactored the tier-2 pipeline to serialize the IR text **before**
`ir_jit_compile()` via `ir_save()` + `open_memstream()`, then reload it into a
fresh `ir_ctx` via `ir_load_safe()` + `ir_loader` callbacks in the tier-2
compiler. The fresh context then runs the full scheduling pipeline
(`ir_build_def_use_lists` → `ir_build_cfg` → `ir_build_dominators_tree` →
`ir_find_loops` → `ir_gcm` → `ir_schedule` → `ir_schedule_blocks`) before
`ir_emit_llvm()`.

This also fixed LLVM "Instruction does not dominate all uses" errors — the
Sea-of-Nodes IR requires GCM (Global Code Motion) to place instructions in
dominating blocks before LLVM emission.

**Additional fixes required:**
- `ir_emit_llvm.c`: 3 null-dereference fixes where `ctx->loader->add_sym` was
  accessed without checking `ctx->loader != NULL`
- `ir_load.c`: Added `ir_load_safe()` — like `ir_load()` but returns 0 on
  parse error instead of calling `exit(2)`
- `tier2_func_process`: Infer `ret_type` from RETURN instruction (ir_save
  doesn't encode it, ir_load sets it to -1)

**Impact:** All 3 previously failing kernels (blake3-scalar, blind-sig,
shootout-nestedloop) now pass tier-2. Total: 37/37 kernels pass.

---

## Bug 12: rust-compression segfault with tier-2 compiled code — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `thirdparty/ir/ir_emit_llvm.c` — SHL/SHR/SAR emission
**Category:** ir_emit_llvm semantic bug (wasm/LLVM shift semantics mismatch)

**Root cause:** `ir_emit_llvm` emitted shift operations (`shl`, `lshr`, `ashr`)
without masking the shift amount. WebAssembly defines all shifts as modular
(shift amount taken mod bit-width), and x86 hardware naturally masks shift
counts (mod 64 for 64-bit, mod 32 for 32-bit). So tier-1 native code worked
correctly even with shift amounts >= bit-width.

However, LLVM IR defines shifts by >= bit-width as producing **poison** (undefined
behavior). At O2, LLVM's optimizer exploits this: it sees a shift amount that is
provably >= 64 and replaces the result with `undef` or optimizes away dependent
code. This caused a tier-2 compiled function (func 239 in rust-compression) to
produce wrong output values, leading to incorrect control flow downstream that
eventually called a NULL FuncTable entry (func 487, a non-JIT-compiled function).

**Specific example:** In function 239, `ir_ctx` constant `c_28 = 4294967295`
(= 0xFFFFFFFF, used as both a shift delta and a 32-bit mask):
```
d_65 = ADD(d_64, 4294967295)   ; d_64 in [0,63], so d_65 > 4 billion
d_66 = SHR(d_60, d_65)         ; shift by >64 bits = LLVM UB!
```
Tier-1 (x86): `SHR reg, cl` masks to `(d_64 + 0xFFFFFFFF) & 63 = d_64 - 1`. Correct.
Tier-2 (LLVM O2): shift by > 64 → poison → wrong results → crash.

**Fix:** Added `ir_emit_shift_op()` in `ir_emit_llvm.c` that emits an `and`
instruction to mask the shift amount before the shift:
- Constant shift amounts: mask folded at emit time
- Variable shift amounts: `%masked = and iN %amt, (bits-1)` emitted before shift

This also fixed the `regex` kernel crash (same root cause, different function).

**Impact:** rust-compression and regex now pass with tier-2. Tier-2 results
improved from 33/37 to 34/37.

---

## Bug 13: Static ALLOCA misclassified as dynamic when not in BB1 — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `thirdparty/ir/ir_x86.dasc`, line ~2698 (`ir_match`, `IR_ALLOCA` case)
**Category:** ir backend bug

**Root cause:** The `ir_match` pass classifies an `ALLOCA` as `IR_STATIC_ALLOCA`
(compiled as a fixed prologue stack expansion) **only if it is in basic block 1**
(`cfg_map[ref] == 1`). If the ALLOCA is in any other block, the library falls
through to set `IR_USE_FRAME_POINTER` and treat it as a dynamic alloca.

The tier-up counter instrumentation emits an `IF/MERGE` in the prologue (counter
check + conditional call to `jit_tier_up_notify`). Everything after the MERGE —
including parameter loads and the wasm body's `ALLOCA` — gets a control dependency
on the MERGE, pushing the ALLOCA into **BB4** (after SCCP/GCM scheduling).

Since `cfg_map[alloca_ref] == 4 != 1`, the ALLOCA is classified as dynamic. The
library then sets `IR_USE_FRAME_POINTER` (uses `rbp` as frame pointer instead of
a general-purpose register), which cascades into a completely different register
allocation that produces **incorrect machine code** for certain functions.

**Symptoms:**
| Opt level | Behavior |
|-----------|----------|
| O0 | Correct output |
| O1 | SIGSEGV crash |
| O2 | Silent wrong output (brotli compressed 168242 vs expected 167110) |

Only 1 of 637 functions in `rust-compression.wasm` was affected (function 83).
Isolated via binary search using `WASMEDGE_IFMERGE_MIN`/`MAX` env-var cutoff.

**Fix:** Relax the static ALLOCA check from "must be in BB1" to "must be in a
non-loop block" (`b == 1 || (b > 0 && ctx->cfg_blocks[b].loop_depth == 0)`).
A constant-size ALLOCA in a non-loop block executes exactly once per function
invocation, so it is safe to compile as a static prologue stack expansion.

**Verification:** After the fix, the standalone `ir` tool produces identical
assembly for func 83 with and without IF/MERGE. The `rust-compression` sightglass
kernel passes at O2 with all functions using IF/MERGE.

See `notes/static_alloca_frame_pointer_bug.md` for full debugging methodology.

---

## Bug 15: ir_load parser rejects MERGE/PHI with >255 inputs — OPEN

**Date:** 2026-04-03
**Status:** Open (ir backend limitation)
**Location:** `thirdparty/ir/ir_load.c`, generated parser code (`count > 255` check)
**Category:** ir backend bug

**Root cause:** The ir_load parser has a hard-coded limit: variable-operand
instructions (MERGE, PHI) with more than 255 inputs trigger a parse error.
`ir_save` faithfully serializes MERGE/255 (255 predecessors), but when PHI
adds 1 to the count (256 > 255), the parser rejects it.

**Affected:** rust-compression func 185 (MERGE with 255 inputs). Only 1 of
~637 functions; the function falls back to tier-1 silently via `ir_load_safe`.

**Impact:** Minimal — single function in one kernel. All other functions
compile to tier-2 normally.

---

## Bug 16: LLVM 18 codegen crash with scalable vector types — MITIGATED

**Date:** 2026-04-03
**Status:** Mitigated (LLVM 18 bug; crashes prevented at process level)
**Location:** LLVM 18 SelectionDAG ISel (libLLVM-18.so)
**Category:** LLVM bug

**Root cause:** LLVM 18's x86-64 SelectionDAG ISel can crash during codegen
when processing LLVM IR produced by `ir_emit_llvm`. Three crash modes:

1. `report_fatal_error("Invalid size request on a scalable vector")` — SIGABRT
   in `TypeSize::operator unsigned long()`, triggered from auto-vectorization
   creating scalable vector types that ISel can't handle.

2. `report_fatal_error("Do not know how to expand/widen/split the result of
   this operator!")` — SIGABRT from ISel encountering unsupported node types.

3. SIGSEGV in `EVT::isExtendedFixedLengthVector()` — null pointer dereference
   in the EVT type set during `SelectionDAGBuilder::visitBr` / `visitLoad`.

All originate in LLVM's internal handling during codegen passes, not from
our IR input (verified: no `vscale` or scalable types in `ir_emit_llvm` output).

**Mitigations applied (not workarounds — correct configuration & shutdown):**

1. **TargetMachine + target triple + datalayout:** `ir_emit_llvm` produces
   LLVM IR without a target triple or datalayout. Previously `LLVMRunPasses`
   was called with `nullptr` TargetMachine, causing `TargetLibraryInfoImpl`
   to operate on uninitialized data (SIGSEGV in `getLibFunc`). Fixed by
   creating a native TargetMachine and setting the module's triple/datalayout.

2. **Disabled auto-vectorization:** Loop/SLP vectorization is disabled via
   `LLVMPassBuilderOptionsSetLoopVectorization(PBO, 0)` and
   `LLVMPassBuilderOptionsSetSLPVectorization(PBO, 0)`. Wasm JIT functions
   are typically small and don't benefit from auto-vectorization. This
   eliminates the "Invalid size request on a scalable vector" crash path.

3. **Shutdown checks in Tier2Compiler:** The compiler checks `Shutdown_` at
   key points between LLVM phases (after optimization, before LLJIT creation,
   before codegen lookup). This allows the worker to bail out before entering
   ISel when process exit has been signaled.

4. **Clean process exit:** The atexit handler calls `Mgr->shutdown()` then
   `_exit(0)`. This terminates the process immediately (no static destructors,
   no further atexit handlers), preventing the worker thread from crashing
   during or after teardown. All test output is already flushed before atexit.

**Result:** 0/38 core dumps across all sightglass kernels (previously 4-8).
Stress-tested hashset (most frequent crasher) 20/20 clean exits.

**Remaining LLVM bug:** The ISel crashes (modes 2, 3) remain latent — they
would trigger if a function compiled during normal execution (not exit time)
hits the bug. In practice this hasn't been observed: tier-up thresholds are
high enough that only a few functions per kernel reach tier-2, and those
have consistently compiled without triggering ISel bugs. The risk is
theoretical for now; upgrading LLVM would eliminate it.

---

## Bug 17: Use-after-free race in Tier2Manager — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `lib/vm/tier2_manager.cpp`, `lib/executor/helper.cpp`
**Category:** WasmEdge bug (our code)

**Root cause:** The Tier2Manager's background worker thread accessed
`ModuleInstance` and `FunctionInstance` objects after they were destroyed
by the main thread during VM teardown. This caused:
- "not an IR JIT function" warnings (accessing destroyed FunctionInstance)
- Intermittent SIGSEGV (writing to freed FuncTable vector data)

**Fix:**
1. Changed `enqueue()` to copy all needed data (IRText, RetType) at call time
   instead of capturing a ModuleInstance pointer
2. Changed `FuncTable` from `std::vector<void*>` to `std::shared_ptr<void*[]>`
   so the allocation stays alive for the background worker
3. Worker thread no longer accesses any ModuleInstance or FunctionInstance

---

## Bug 18: ret_type not surviving ir_save/ir_load round-trip — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `lib/vm/ir_jit_engine.cpp`, `lib/vm/tier2_compiler.cpp`,
  `include/runtime/instance/function.h`
**Category:** Pipeline bug (ir backend limitation + our code)

**Root cause:** `ir_save()` does not encode `ctx->ret_type`. After
`ir_load()` in the tier-2 pipeline, `ctx->ret_type` is set to `-1`
(unset). When `ir_emit_llvm()` generates the LLVM function signature,
it uses `ret_type` to determine the return type. With `-1`, void-returning
functions got `ret i64` instead of `ret void`, causing LLVM verification
errors: "value doesn't match function result type."

**Fix:** Capture `ctx->ret_type` from the tier-1 `ir_ctx` before
`ir_jit_compile()` (which destroys the context). Thread it through:
`CompileResult::RetType` → `IRJitFunction::RetType` → `Tier2Manager::Request`
→ `Tier2Compiler::compile()`. The tier-2 loader callback restores
`ctx->ret_type` from this saved value.

**Affected:** blind-sig, rust-compression, rust-html-rewriter (functions
with void return type).

---

## Bug 19: Exit-time core dump (worker thread mid-LLVM at teardown) — FIXED

**Date:** 2026-04-03
**Status:** Fixed
**Location:** `lib/executor/helper.cpp`, `lib/vm/tier2_manager.cpp`,
  `lib/vm/tier2_compiler.cpp`, `include/vm/tier2_compiler.h`,
  `include/vm/tier2_manager.h`
**Category:** WasmEdge bug (our code) + LLVM 18 interaction

**Root cause:** Three interacting issues caused exit-time core dumps:

1. **Missing TargetMachine in Tier2Compiler:** `LLVMRunPasses` was called with
   `nullptr` TargetMachine. `ir_emit_llvm` produces LLVM IR without a target
   triple or datalayout. This caused `TargetLibraryInfoImpl::getLibFunc` to
   `memcmp` against uninitialized data → SIGSEGV during optimization passes.

2. **LLVM 18 auto-vectorization creating scalable vectors:** Loop/SLP
   vectorization passes created scalable vector types that LLVM 18's x86-64
   ISel couldn't handle → `report_fatal_error("Invalid size request on a
   scalable vector")` → SIGABRT.

3. **Worker thread outliving process exit:** The Tier2Manager was intentionally
   leaked (never destroyed). When `main()` returned, the worker thread could be
   mid-LLVM-compilation. Two failure modes:
   - LLVM ISel bug triggers (crash modes 1, 2) → process-fatal signal
   - LLVM statics destroyed by C++ static destructors while worker still uses
     them → use-after-free

**Symptoms:** 4-8 out of 38 sightglass kernels would core dump AFTER printing
`[PASSED]`. Exit code 139 (SIGSEGV) or 134 (SIGABRT). Intermittent — depended
on timing of worker thread vs process exit.

**Fix (four coordinated changes):**

1. **TargetMachine:** Create a native `LLVMTargetMachineRef` in
   `Tier2Compiler::Impl`. Set module target triple and datalayout before
   optimization. Pass the TM to `LLVMRunPasses`.

2. **Disable auto-vectorization:** Call
   `LLVMPassBuilderOptionsSetLoopVectorization(PBO, 0)` and
   `LLVMPassBuilderOptionsSetSLPVectorization(PBO, 0)`. Wasm JIT functions are
   small and don't benefit from auto-vectorization.

3. **Shutdown checks in compiler:** `Tier2Compiler` accepts a
   `std::atomic<bool>*` shutdown flag via `setShutdownFlag()`. The compiler
   checks it at 3 points (after optimization, before LLJIT creation, before
   codegen lookup) and bails out if set.

4. **Clean process exit via `_exit(0)`:** The atexit handler calls
   `Mgr->shutdown()` (sets `Shutdown_`, notifies CV) then `_exit(0)`.
   `_exit` terminates the process immediately — no static destructors, no
   further atexit handlers — so the worker thread is killed cleanly by the OS.
   All test output is already flushed by this point.

**Result:** 0/38 core dumps. Stress-tested hashset (worst case) 20/20 clean
exits. All kernels pass with exit code 0.

**Design rationale:** `_exit(0)` was chosen over `join()` because LLVM ISel
bugs are process-fatal (SIGSEGV/SIGABRT kill all threads). No amount of
join/timeout logic can prevent a crash signal from another thread. `_exit(0)`
preempts the crash by terminating before LLVM reaches the buggy codegen path.
This is not an error handler — it's a shutdown strategy.

---

## Bug 14 (known, pre-existing): O1 crash with IF/MERGE in prologue

**Date:** 2026-04-03
**Status:** Open (pre-existing ir backend bug)

**Description:**
`rust-compression` crashes at O1 with IF/MERGE in the prologue (tier-up counter
check). O1 does not run SCCP. Not related to the ALLOCA classification issue
(Bug 13). Separate root cause.

**Workaround:** Skip O1 when testing (per project rules).
