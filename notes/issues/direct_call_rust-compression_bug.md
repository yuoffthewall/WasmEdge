# Bug: rust-compression brotli output mismatch under PLT direct calls (IR JIT O2)

**Branch:** `direct_call`
**Date:** 2026-04-09 (spans two debug sessions)
**Status:** Root cause narrowed but not yet fixed

---

## 1. Syndrome

When PLT-style direct call stubs are enabled for JIT-to-JIT calls, the `rust-compression` Sightglass kernel produces **wrong brotli compression output** under IR JIT O2.

- **Expected:** `brotli compressed: 167110 bytes (9.7%)`
- **Actual:**   `brotli compressed: 168242 bytes (9.7%)`
- **Delta:** +1132 bytes (~0.67% larger). The output is still valid compressed data, but the compression decisions are subtly wrong -- consistent with a data-flow error inside the compressor's main loop.
- **gzip output is correct** (192012 bytes in both cases). Only brotli is affected.
- **O0 passes.** The bug is in the O2 optimisation/register-allocation path, not in the PLT stub machine code.
- **All other Sightglass kernels are untested** with PLT stubs in this session.

---

## 2. How to reproduce

### Prerequisite: debug instrumentation in `ir_builder.cpp`

The `WASMEDGE_PLT_CALLER` / `WASMEDGE_PLT_CALLEE_MIN` env-var guards (currently checked in on the `direct_call` branch) must be present to restrict PLT usage to a single caller.

### Minimal failure

```bash
cd build
WASMEDGE_SIGHTGLASS_MODE=IR_JIT    \
WASMEDGE_IR_JIT_OPT_LEVEL=2        \
WASMEDGE_SIGHTGLASS_KERNEL=rust-compression \
WASMEDGE_PLT_CALLER=83             \
timeout 15 ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
# => FAILED  (stderr mismatch vs .expected)
```

### Passing (PLT disabled)

Remove `WASMEDGE_PLT_CALLER=83` or set it to a non-existent function index:

```bash
WASMEDGE_PLT_CALLER=9999 ...  # same command above => PASSED
```

### Standalone RA reproduction (no WasmEdge runtime needed)

```bash
cd thirdparty/ir
# PLT-on IR (failing RA pattern):
./ir -O2 -mfastcall /tmp/plt_on/wasmedge_ir_071_after.ir -S >/dev/null 2>/tmp/fc_on.asm

# PLT-on IR with r15 excluded (passing RA pattern):
./ir -O2 -mfastcall --debug-regset 0xFFFF7FFF /tmp/plt_on/wasmedge_ir_071_after.ir -S >/dev/null 2>/tmp/fc_on_no_r15.asm

# Full RA debug trace:
./ir -O2 -mfastcall --debug-regalloc /tmp/plt_on/wasmedge_ir_071_after.ir -S >/dev/null 2>/tmp/ra_debug_on.txt
```

`-mfastcall` is **required** to match WasmEdge's `IR_CC_FASTCALL` calling convention. Without it, the standalone tool produces different code.

---

## 3. Root cause (narrowed, not pinpointed)

### 3.1 The trigger

The PLT direct-call optimisation replaces, per call site:

```
LOAD env->FuncTable -> LOAD FuncTable[i] -> PROTO -> CALL (indirect)
```

with:

```
FUNC_ADDR(plt_stub_addr) -> CALL (direct)
```

This removes **3 IR nodes per call site** (2 call sites to callee 559 in function 83 = 6 fewer nodes). The reduced node count lowers register pressure near the call sites, which changes the LSRA's split/spill decisions.

### 3.2 The cascade

In the IR library's Linear Scan Register Allocator (`thirdparty/ir/ir_ra.c`):

| | PLT-off (passing) | PLT-on (failing) |
|---|---|---|
| R7 (d_12) split at pos 758.1 | **YES** -- gets edx [536.0..758.1), then spilled | **NO** -- gets r15d for [536.0..1188.2), spilled [1188.2..1559.0), r15d again [1559.0..1572.1) |
| Subsequent cascade | ~40+ intervals get different registers | Different registers throughout |

The split decision at position 758.1 is the **origin of all register-assignment differences**. Every downstream allocation is affected.

### 3.3 The symptom: register-assignment sensitivity

Excluding **any single callee-saved register** (rbx, rbp, r12, r13, r14, or r15) from allocation for function 83 makes the test **pass**. Excluding rcx also passes. Excluding any other caller-saved register (rax, rdx, rsi, rdi, r8-r11) still **fails**.

| Excluded register | Result |
|---|---|
| none | FAIL |
| rbx (3) | PASS |
| rbp (5) | PASS |
| r12 (12) | PASS |
| r13 (13) | PASS |
| r14 (14) | PASS |
| r15 (15) | PASS |
| rcx (1) | PASS |
| rax (0) | FAIL |
| rdx (2) | FAIL |
| rsi-r11 (6-11) | FAIL |

This proves the bug is **not r15-specific**. Rather, the specific register allocation produced when all registers are available has a semantic error. Excluding any callee-saved register (or rcx) perturbs the allocation enough to avoid the buggy pattern.

### 3.4 What has been verified correct

- **PLT stub machine code** (`ir_jit_engine.cpp:392-408`): `mov rax,[rdi]; mov rax,[rax+i*8]; jmp rax`. Only clobbers rax (caller-saved). Verified by disassembly.
- **Prologue/epilogue**: Push/pop sequences match. Stack frame total = 408 bytes in both versions. Callee-saved registers are correctly saved/restored.
- **d_12 spill slot (0x50(%rsp))**: Written once at function entry, never overwritten. All reloads read from this slot. Spill handling is correct.
- **DESSA moves at loop backedge (.L20)**: The parallel-copy sequence `load old -> store to new slot -> store computed value` is structurally correct and identical between versions.
- **Codegen IR (instruction selection + scheduling)**: Identical between failing and passing versions when register names are stripped.
- **Callee chain**: Function 559 and its callees do not use or clobber r15.
- **REX prefix encoding**: Binary encoding of r15 instructions is correct in the JIT output.

### 3.5 What is NOT yet proven

The **exact instruction or value** that computes a wrong result has not been identified. The entire codegen structure (instruction selection, scheduling, DESSA moves) is identical between failing/passing -- only register names differ. The bug must be one of:

1. A **spill-slot collision** where two live values share a stack slot when they shouldn't (the RA allocates spill slots, and the specific allocation pattern may create an overlap), OR
2. A **missed spill/reload** where the RA assumes a value is still in a register but it was clobbered by a different allocation, OR
3. A **code emission bug** in `ir_x86.dasc` or `ir_emit.c` that produces wrong machine code for a specific multi-register instruction pattern (e.g., a `mov Rd(reg), imm` or LEA pattern that encodes incorrectly with certain register combinations), OR
4. A **DESSA move ordering error** that only manifests with this specific register assignment (a cycle in the parallel copy that isn't resolved correctly).

**Speculation (labeled):** The most likely candidate is a spill-slot reuse conflict. The RA trace shows spill slot sharing (e.g., R11 and R89 share 0x64, R36 and R97 share 0xa4). With the failing allocation, a shared slot may be written by one value while the other is still live. The subtle output difference (slightly wrong compression, not garbage) is consistent with an occasionally-stale value read from a shared spill slot.

---

## 4. Fix

**No fix yet.** The root cause is narrowed to the register allocation pattern but the exact faulty instruction/slot has not been identified.

### Workaround (currently active)

In `ir_jit_engine.cpp`:

```cpp
// Exclude a callee-saved register for function 83 to force a different RA pattern
if (_dbg_cur_id == 71) Ctx->fixed_regset |= (1ULL << 15);
```

This forces the allocator to avoid r15 for function 83, which changes the allocation cascade enough to avoid the buggy pattern. This is a **hack**, not a fix.

### Suggested next steps (priority order)

1. **GDB execution comparison**: Run both failing (no exclusion) and passing (exclude rbx) under GDB, break at `wasm_jit_071`, single-step through the main brotli loop, and find the first iteration where a register value diverges. This is the most direct path to finding the exact faulty instruction.

2. **Spill-slot collision audit**: For each pair of values sharing a spill slot in the failing RA trace (`ra_debug_on.txt`), verify that their live ranges truly don't overlap. A conflict would be the smoking gun.

3. **Standalone minimal repro**: Extract the failing function's IR and use the standalone tool's `--run` flag (if feasible) to execute both versions and compare outputs without the full WasmEdge runtime.

4. **IR library LSRA code review**: Focus on `ir_ra.c` around spill-slot allocation and the `needs_spill_reload()` function. The commit `21b6eea` fixed a related `ir_is_dead_load()` bug -- the current bug may be in the same area.

### Files with debug instrumentation (TO REVERT before any commit)

| File | What to revert |
|---|---|
| `lib/vm/ir_builder.cpp` | `WASMEDGE_PLT_CALLER` / `WASMEDGE_PLT_CALLEE_MIN` env var guards; `spdlog::info` for func 83 |
| `lib/vm/ir_jit_engine.cpp` | `WASMEDGE_DBG_EXCL_REG` register exclusion; native code binary dump to `/tmp/wasmedge_ir_NNN_code.bin` |
| `test/ir/ir_benchmark_test.cpp` | Actual-stderr dump to `/tmp/actual_stderr_*.bin` |

---

## Artifacts

All at `/tmp/` (ephemeral):

| Path | Description |
|---|---|
| `plt_on/wasmedge_ir_071_after.ir` | Function 83 IR with PLT stubs (failing input) |
| `plt_off/wasmedge_ir_071_after.ir` | Function 83 IR without PLT stubs (passing input) |
| `fc_on_fresh.asm` | Standalone assembly, PLT-on, all registers |
| `fc_on_no_r15.asm` | Standalone assembly, PLT-on, r15 excluded |
| `ra_debug_on.txt` | Full LSRA trace, PLT-on (5552 lines) |
| `ra_debug_on_no_r15.txt` | Full LSRA trace, PLT-on, r15 excluded |
| `codegen_on_fresh.ir` | Codegen IR with register assignments (failing) |
| `codegen_excl_rbx.ir` | Codegen IR with rbx excluded (passing) |
| `actual_stderr_rust-compression_IR_JIT.bin` | Actual wrong output from failing run |
| `wasmedge_ir_071_code.bin` | Raw JIT binary for function 83 |

---

## Dead ends (do not retry)

| Attempt | Result |
|---|---|
| `ir_const_func` (named symbol) vs `ir_const_func_addr` (direct address) | Both fail identically |
| `MAP_32BIT` for stubs (force `call rel32`) | Still fails -- rax clobber theory is wrong |
| O0 with PLT stubs | Passes -- bug is O2-specific |
| Constant pool size increase (256->8192) | Not the cause |
| SCCP realloc bug | Function 83 has only ~445 constants |
| `ir_match_builtin_call` interference | Only triggers for `IR_CC_BUILTIN` |
| Standalone cmov condition difference (cmovel vs cmovnel) | Semantically equivalent |
| SCCP producing different IR for PLT-on vs PLT-off | Confirmed identical |
| r15-specific encoding / REX prefix bugs | Not the issue (all callee-saved exclusions fix it) |
| Epilogue offset miscalculation for r15 | Verified correct |
| d_12 spill slot (0x50) corruption | Slot is never overwritten |
