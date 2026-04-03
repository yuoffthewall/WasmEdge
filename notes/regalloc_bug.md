# IR Library Call-Emission Bugs — Root Causes and Fixes

## Overview

After introducing the register-based calling convention (`__fastcall` / SysV ABI
parameter registers) for direct JIT-to-JIT calls, three bugs were found and
fixed. One remains open.

| Bug | Opt level | File | Status |
|-----|-----------|------|--------|
| #1 — stk_tmp clobbers register arg (O0) | O0 | ir_x86.dasc | Fixed |
| #2 — prev_use_ref skips spill-reload for duplicate args | O2 | ir_ra.c | Fixed |
| #3 — ir_UNREACHABLE drops function epilogue | O0–O2 | ir_builder.cpp, helper.cpp, ir_jit_engine.cpp | Fixed |
| rust-compression O2 regression | O2 | thirdparty/ir (upstream) | Open |

---

## Background: How `ir_emit_arguments` Works

Bugs #1–#2 are in or around `ir_emit_arguments()` in `ir_x86.dasc`. This
function sets up CALL arguments in three passes:

1. **Pass 1** — Scan all arguments. For register args already in the right
   register: nothing. For register args in a *different* register: collect a
   REG→REG copy for pass 2. For spilled/constant register args: defer to pass 3.
   For stack args already in a register: store directly to the outgoing stack
   frame. For spilled/constant stack args: defer to pass 3.

2. **Pass 2** — `ir_parallel_copy()` resolves all REG→REG moves (handles cycles
   using `tmp_reg` as a swap register).

3. **Pass 3** — Load remaining values from spill slots / constants into their
   target registers (register args) or onto the outgoing stack frame (stack
   args). Stack args use a dedicated scratch register (`stk_tmp`) to load-then-
   store.

---

## Bug #1 — `stk_tmp = tmp_reg` Clobbers Register Arg at O0

### Symptom

At O0, four Sightglass kernels failed: `blake3-scalar`, `gcc-loops`,
`pulldown-cmark`, `shootout-ackermann`. `shootout-ackermann` output `M = 0 and
N = 0` instead of `M = 3 and N = 7`.

### Root Cause

**File:** `thirdparty/ir/ir_x86.dasc`, function `ir_emit_arguments()`, pass 3.

Pass 3 stack-argument copies originally used `tmp_reg` as scratch. The CALL
instruction's constraint system (`ir_get_target_constraints`) allocates a
temporary register at slot 1 for indirect calls. `ir_emit_call` passes
`ctx->regs[def][1]` as `tmp_reg` to `ir_emit_arguments`. At O0, the simple
allocator assigned **RCX** to this slot.

The problem: pass 3 processes register args first (loading from spill slots into
RDI, RSI, RDX, **RCX**, R8, R9), then processes stack args using `tmp_reg`
(= RCX) for load-then-store. The stack-arg setup **clobbered the 4th register
argument** (RCX) that was already loaded:

```asm
;; Pass 3 register arg setup:
mov    0x2c(%rsp),  %ecx        ; arg3 = p1 (correct value in RCX)
mov    0x170(%rsp), %r8d        ; arg4 = d_94
mov    0x190(%rsp), %r9         ; arg5 = d_99

;; Pass 3 stack arg setup — CLOBBERS ECX:
mov    0x188(%rsp), %rcx        ; tmp_reg=RCX: loading d_98 for stack
mov    %rcx,        (%rsp)      ; → stack arg 0  (ECX now = d_98, not p1!)
...
call   *0x1b0(%rsp)             ; ECX = wrong value
```

### Fix

Introduced a dedicated `stk_tmp = IR_REG_RAX` for stack-argument copies instead
of reusing `tmp_reg`. RAX is never a parameter register on SysV or Windows
x86-64, so it cannot conflict with register arguments loaded earlier in pass 3.

```c
// Before (broken):
// used tmp_reg directly — could be any scratch reg (RCX at O0)

// After (fixed):
ir_reg stk_tmp = IR_REG_RAX;
ir_emit_load(ctx, type, stk_tmp, arg);
ir_emit_store_mem_int(ctx, type, mem, stk_tmp);
```

The original `tmp_reg` is still passed to `ir_parallel_copy` in pass 2, where
it is used as a swap register for resolving register copy cycles. This avoids
any regression at O1/O2.

---

## Bug #2 — `prev_use_ref` Skips Spill-Reload for Same-Instruction Duplicate Args

### Symptom

`shootout-ed25519` crashes at O2 in `wasm_jit_003` with a SIGSEGV on an
out-of-bounds memory access. The caller (`wasm_jit_002`) passes garbage in a
parameter register.

### Root Cause

**File:** `thirdparty/ir/ir_ra.c`, function `assign_regs()` (line ~3896).

The LSRA walks each live interval's use positions and decides whether the value
needs a spill-reload (`IR_REG_SPILL_LOAD`) at each use site. As an
optimization, it tracks `prev_use_ref` — the last instruction that used this
interval — and skips the `needs_spill_reload()` check when the current use is
in the same basic block as the previous use (since `needs_spill_reload` works at
block granularity, not instruction granularity).

The bug: when the **same variable appears twice as different operands of the same
CALL instruction**, both uses share the same `ref`. On the first use,
`needs_spill_reload()` correctly returns true (a prior CALL in the block
clobbered the caller-saved register), and the allocator sets
`reg |= IR_REG_SPILL_LOAD`, then sets `prev_use_ref = ref`.

On the **second** use at the same CALL, `prev_use_ref == ref`, so
`ctx->cfg_map[prev_use_ref] == ctx->cfg_map[ref]`, the entire condition
short-circuits to **false**. The allocator falls through to the "reuse register
without spill load" path, assigning a **plain register** (no SPILL_LOAD flag).

### Fix

**File:** `thirdparty/ir/ir_ra.c`, line 3896.

Added `prev_use_ref == ref` to force re-evaluation of `needs_spill_reload` when
two uses of the same variable are at the **same instruction**:

```c
// Before (broken):
if ((!prev_use_ref || ctx->cfg_map[prev_use_ref] != ctx->cfg_map[ref])
 && needs_spill_reload(ctx, ival, ctx->cfg_map[ref], available)) {

// After (fixed):
if ((!prev_use_ref || prev_use_ref == ref || ctx->cfg_map[prev_use_ref] != ctx->cfg_map[ref])
 && needs_spill_reload(ctx, ival, ctx->cfg_map[ref], available)) {
```

---

## Bug #3 — `ir_UNREACHABLE` Drops Function Epilogue + Error Swallowed

### Symptom

`rust-compression` hits Wasm `unreachable` traps (error code 0x40a) at bytecode
offsets 0x73e09 and 0x76e1a. These traps are silently swallowed and execution
continues with corrupt state, eventually causing a SIGSEGV in the interpreter
(`getOpCode(this=0x20)`).

This is actually two bugs working together:

### Bug #3a — `ir_UNREACHABLE` Generates No Code After CALL

**File:** `lib/vm/ir_builder.cpp`, Wasm `unreachable` handler.

The IR builder translated Wasm `unreachable` to `ir_UNREACHABLE()`. In the IR
framework, `UNREACHABLE` is a terminator used after tail calls — it tells the
code emitter that the successor block is dead code and no epilogue is needed.

But our JIT functions use regular `CALL` (not tail call) to invoke
`jit_host_call`. A regular call returns normally. When the IR framework sees
`UNREACHABLE` as the successor, it marks the block as dead and generates no
code after the CALL. Execution falls through into uninitialized memory (zeros
= `add %al,(%rax)` → SIGSEGV when rax=0).

**Example:** `wasm_jit_401` (function 413, which calls skipped function 512):
```
IR:   CALL/3(hostCallFn, env, 512, 0)
      UNREACHABLE

ASM:  sub    $0x8,%rsp
      mov    %rdi,%rax
      mov    %rax,%rdi
      mov    $0x200,%esi    ; funcIdx=512
      xor    %rdx,%rdx
      call   *0x20(%rax)    ; jit_host_call
      ; NO RET — falls through into zeros → crash
```

**Fix:** Changed `ir_UNREACHABLE()` to `ir_RETURN(getOrEmitReturnValue())`. This
ensures the IR backend always generates a proper function epilogue (stack
restore + `ret`). The Wasm-level trap is still handled by the interpreter when
reached through the host-call trampoline.

### Bug #3b — `jit_host_call` Swallows Callee Errors

**File:** `lib/executor/helper.cpp`, `lib/vm/ir_jit_engine.cpp`.

When `jit_host_call` invokes a function that traps (e.g., hits `unreachable`),
`jitCallFunction()` returns an error. The old code only handled `Terminated`
errors (via `longjmp(buf, 1)`); all other errors were silently swallowed and
`jit_host_call` returned 0. The JIT caller continued executing with potentially
wrong state.

**Fix:** `jit_host_call` now `longjmp(buf, 3)` on any callee error, stashing
the error code in thread-local storage. The top-level `invoke()` handles
`jmpVal==3` by retrieving the stashed error code and returning it as a proper
`Unexpect`:

```cpp
// jit_host_call (helper.cpp):
if (!res) {
    void *buf = wasmedge_ir_jit_get_termination_buf();
    if (buf) {
        if (res.error() == ErrCode::Value::Terminated)
            longjmp(*buf, 1);
        wasmedge_ir_jit_set_callee_error(res.error());
        longjmp(*buf, 3);   // unwind entire JIT stack
    }
}

// invoke() (ir_jit_engine.cpp):
if (jmpVal == 3)
    return Unexpect(wasmedge_ir_jit_get_callee_error());
```

### Additional Fix — Skipped Function NULL Vtable Entry

Skipped functions (those starting with Wasm `unreachable`) have NULL entries in
the JIT function table. Before Bug #3 was identified, direct inline calls to
skipped functions crashed by jumping to NULL. Fixed by:

1. Passing the `SkipJit` vector to the IR builder
   (`IRBuilder.setSkippedFunctions(SkipJit)` in `module.cpp`)
2. Routing calls to skipped functions through `jit_host_call` (buffer-based ABI)
   instead of direct register-based calls (`IsHostCall` check in
   `ir_builder.cpp`)
3. Including skipped functions in the `MaxCallArgs` pre-scan (since they now
   use the buffer path)

---

## Open Issue — `rust-compression` O2 Regression

### Status: Under Investigation

`rust-compression` hits `unreachable` trap (error 0x40a) at O2. Passes at O0
and O1.

### Failing Call Chain (at crash time)

```
funcIdx 592 → 572 → 466 → 441 → 442 → 474 → 478 → 487 (unreachable trap)
```

This chain does NOT include the suspect functions (577/578/615). The bug
corrupts **wasm linear memory**; downstream functions read wrong values and
branch to the trap stub (funcIdx 487).

### Suspect Functions

Skipping any one of these (via `WASMEDGE_IR_JIT_SKIP`) fixes the bug:

| funcIdx | func_id | Wasm params | Role |
|---------|---------|-------------|------|
| 577 | 563 | 3 × i32 | `itoa`-like: converts number to digit bytes in memory. Calls 578 with 6 wasm args via register ABI (`CALL/7` including exec_env). |
| 578 | 564 | 6 × i32 | Large function with SWITCH, multiple `call_indirect` trampolines, and 3 direct calls to funcIdx 615 via register ABI (`CALL/6`). |
| 615 | 601 | 5 × i32 | Small helper with 2 `call_indirect` paths. Called **only** from 578. |

### func_id ↔ funcIdx Mapping

`ImportFuncNum = 12`, 2 functions skipped (funcIdx 487 at CodeIdx 475, funcIdx
512 at CodeIdx 500). For func_id N: `funcIdx = N + 12 + (number of skips with
CodeIdx ≤ N + skips)`.

### What Was Ruled Out

- **Parameter count mismatch** — Caller/callee agree on param counts for all
  three functions.
- **7th arg (stack-passed) alignment** — Correctly located at `0xb0(%rsp)` in
  the callee (6 pushes × 8 + `sub $0x78` + return address = 0xb0).
- **Return type mismatch** (caller PROTO declares `IR_I64`, callee compiled as
  `IR_I32`) — Changing the caller's PROTO to `IR_I32` to match the callee did
  NOT fix the bug. The TRUNC in the disassembly correctly uses `test %eax,%eax`
  (32-bit), not `test %rax,%rax`.
- **Prologue register saves** — All callee-saved registers properly saved.
  Short-lived params (e.g. p1/EDX in funcIdx 615) consumed before clobbering.
- **ALLOCA overlap with spill slots** — ALLOCA at RSP+0 (24 bytes), spills
  start at RSP+0x18 — no overlap.
- **Forcing all calls to buffer-based ABI** — Broke unrelated functions
  (`ir_check` assertion: `SharedCallArgs` not allocated for functions that had
  no buffer-based calls originally).

### Key Observations

- **O0: PASS, O1: PASS, O2: FAIL** — The IR framework's O2-specific
  optimizations (likely LSRA register allocation or instruction scheduling)
  produce incorrect codegen for one of these functions.
- The `thirdparty/ir` library has had multiple recent O2 regalloc fixes (TRUNC
  stale bits, LSRA eviction, dead PHI clobbering, stale EFLAGS) — this may be
  another instance of a similar class of bug in the IR backend.
- funcIdx 615 is called **only** from funcIdx 578. Skipping 615 prevents the
  crash — but 615 is only reached when 578 takes a specific conditional path
  (`d_152 = ULE(d_146, d_140)` in BB23). During correct execution this path may
  not be taken; wrong values computed at O2 could flip the condition.

### Current Key Findings (updated 2026-04-04)

**ALLOCA-overlap hypothesis disproven.** Earlier analysis suspected the
SharedCallArgs ALLOCA buffer at `%rsp` overlapped with spill slots. This was
wrong:

- The ALLOCA for funcIdx 474 is **16 bytes** (c_13 = 16, i.e., MaxCallArgs = 2).
  Only 2 values are stored to the buffer before the `jit_call_indirect` call.
- `ecx = 7` is the **typeIdx** argument to `jit_call_indirect`, NOT the number
  of args in the buffer.
- `rsp+0x24` is **NOT a spill slot** — it is the **parameter p3 save** from the
  function prologue (`mov %r8d, 0x24(%rsp)` at wasm_jit_462+50). The IR
  framework's static ALLOCA and spill allocations do not overlap.

**Actual mechanism:** funcIdx 474 (wasm_jit_462) branches on its 5th wasm
parameter `d_6` ("p3"):

```
l_263 = IF(l_262, d_6);       // IR line 322 — branch on p3
l_264 = IF_TRUE(l_263);       // p3 != 0 → call funcIdx 478
l_265 = END(l_264);
l_266 = IF_FALSE(l_263);      // p3 == 0 → normal path
```

At O2, the caller (funcIdx 454 = wasm_jit_442) passes **p3 = 1**. At O0,
funcIdx 474 is **never called with p3 != 0** (confirmed via conditional GDB
breakpoint on `$r8d != 0` at wasm_jit_462 entry — no hit at O0).

The p3 value comes from a **byte loaded from wasm linear memory** by funcIdx 454
(double indirection: load a wasm pointer from memory, then load a byte at
pointer + 8). This means some **earlier O2-compiled function writes wrong data
to wasm memory**, and the corruption cascades through the call chain.

**Binary search approach (started, not completed):** Skipping funcIdx ranges
via `WASMEDGE_IR_JIT_SKIP` to isolate the buggy function. Initial attempts with
large ranges (half the functions) produced core dumps rather than clean
pass/fail, suggesting the approach needs refinement (e.g., smaller ranges, or
only skip functions not in the critical call path).

### Known Contributing IR Backend Bugs

Two upstream IR backend bugs were identified in this function's crash path.
Fixes were attempted but reverted because they caused regressions in blake3-scalar
and shootout-ed25519. The root cause analysis is correct; the fixes need better
implementations:

- **stk_tmp=RAX clobbers fused call target** (`ir_x86.dasc`): In functions with
  >6 args AND a fused memory call target (`call *offset(%rax)`), the Bug #1 RAX
  scratch clobbers the fused base register. Only affects `wasm_jit_330` among
  sightglass kernels. Attempted R11 fix clobbered the function table pointer.
- **Within-block CALL clobbers caller-saved register** (`ir_ra.c`): When a
  spilled variable is used at two different CALLs in the same basic block, the
  `prev_use_ref` optimization skips reload for the second use. Affects
  `wasm_jit_070`. Attempted scan-based fix destabilized LSRA elsewhere.

### Suggested Next Steps

1. **Hardware watchpoint** — Set a GDB hardware watchpoint on the exact wasm
   memory byte that funcIdx 454 reads as 1 (should be 0). This would directly
   identify which O2-compiled function writes the wrong value.
2. **Refined binary search** — Use `WASMEDGE_IR_JIT_SKIP` with smaller ranges
   (quarters, then eighths) to isolate the one function whose O2 codegen
   corrupts wasm memory. Avoid skipping functions in the critical call path
   (442, 454, 474, 478).
3. **Suspect functions still relevant** — Skipping funcIdx 577, 578, or 615
   individually still fixes the crash. These functions involve `call_indirect`
   and complex control flow (SWITCH) — prime candidates for O2 regalloc bugs.
4. **Revisit the two upstream bugs** — For the RAX/fused-target bug, find a
   register guaranteed free during call emission (not R11). For the within-block
   CALL bug, integrate CALL-awareness into `needs_spill_reload()` itself rather
   than patching the caller.

### Dump File Locations

| File | Content |
|------|---------|
| `/tmp/wasmedge_ir_563_after.ir` | funcIdx 577 O2 IR |
| `/tmp/wasmedge_ir_564_after.ir` | funcIdx 578 O2 IR |
| `/tmp/wasmedge_ir_601_after.ir` | funcIdx 615 O2 IR |
| `/tmp/wasm_jit_563_disas.txt` | funcIdx 577 O2 x86 disassembly |
| `/tmp/wasm_jit_564_disas.txt` | funcIdx 578 O2 x86 disassembly |
| `/tmp/wasm_jit_601_disas.txt` | funcIdx 615 O2 x86 disassembly |
