# IR Library (dstogov/ir) Bugs Encountered in WasmEdge IR JIT

Bugs discovered while inlining direct JIT-to-JIT calls (bypassing the
`jit_direct_or_host` C++ trampoline).  All three are latent bugs in the
IR library that were exposed by the new IR patterns.

---

## Bug 1: SCCP heap-use-after-free on constant-pool realloc

**Syndrome**:
`ir_cfg.c:305: ir_build_cfg: Assertion '((ir_op_flags[insn->op] & (1<<12)) != 0)' failed`
during `ir_jit_compile` at O2.  The assertion is a *secondary* symptom;
the real error is a heap-use-after-free that corrupts the IR graph,
causing `ir_build_cfg` to encounter a non-BB-start instruction where it
expects one.  The crash is non-deterministic and depends on heap layout
(e.g., setting the unrelated env var `WASMEDGE_SIGHTGLASS_QUICK=1`
changes allocations enough to mask it).

**Reproducer** (before the workaround):
```
WASMEDGE_SIGHTGLASS_KERNEL=regex WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

**Root cause** (confirmed with ASan):
The SCCP pass function `ir_promote_i2i` (`ir_sccp.c:1920`) holds a raw
pointer (`insn`) into the instruction buffer `ctx->ir_base`.  It then
calls `ir_const()` to create a new constant.  If the constant pool is
full, `ir_const` -> `ir_const_ex` -> `ir_next_const` -> `ir_grow_bottom`
calls `realloc()`, which may move `ctx->ir_base` to a new address.  The
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

**Why our change exposed it**:
The old code reused a single function pointer (`DirectOrHostFn`, loaded
once at function entry) for all direct calls, requiring few constants per
function.  The new code creates a unique `ir_CONST_ADDR(funcIdx *
sizeof(void*))` per call site.  For large Wasm functions with many calls,
this exhausts the initial 4-entry constant pool (`IR_CONSTS_LIMIT_MIN`),
triggering the realloc during SCCP.

**Workaround** (in `ir_builder.cpp`):
Pre-allocate a 256-entry constant pool instead of the minimum 4:
```cpp
ir_init(&Ctx, ir_flags, 256, IR_INSNS_LIMIT_MIN);
//                       ^^^ was IR_CONSTS_LIMIT_MIN (= 4)
```
This avoids the realloc during optimization.

**Proper fix** (upstream):
`ir_promote_i2i` (and likely other SCCP helpers) must reload their
`insn` pointer from `ctx->ir_base[ref]` after any call that may create
constants.  Alternatively, `ir_grow_bottom` could use a growth strategy
that never moves the buffer (e.g., a linked-list of pages).

---

## Bug 2: x86 address-fusion assertion with ir_MUL_A pattern

**Syndrome**:
`ir_x86.dasc:1871: ir_match_fuse_addr_all_useges: Assertion '((insn->type) < IR_DOUBLE) && ir_type_size[insn->type] >= 4' failed`
during `ir_jit_compile` at O2.

**Reproducer** (before the workaround):
```
WASMEDGE_SIGHTGLASS_KERNEL=pulldown-cmark WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

**Root cause**:
The x86 backend's address-fusion pass (`ir_match_fuse_addr_all_useges`)
tries to fold address arithmetic into x86 addressing modes
(base + index*scale + disp).  When the IR contains
`ir_MUL_A(CONST_ADDR(funcIdx), CONST_ADDR(8))` fed into a `LOAD_A`, the
fusion pass walks the operand chain and encounters a `TRUNC` to `uint8_t`
(IR type size 1) in a surrounding instruction.  The assertion requires
`ir_type_size[insn->type] >= 4`, which fails for 1-byte types.

The `TRUNC` to `uint8_t` exists in the baseline IR too (from Wasm
`i32.store8` patterns), but the old call pattern (5-arg `CALL` through a
single reused pointer) didn't create the `MUL_A` + `ADD_A` chain that
causes the fusion pass to walk into those instructions.

**Workaround** (in `ir_builder.cpp`):
Pre-compute the FuncTable byte offset as a single constant instead of
using `ir_MUL_A`:
```cpp
// Old: ir_MUL_A(ir_CONST_ADDR(funcIdx), ir_CONST_ADDR(sizeof(void*)))
// New: ir_CONST_ADDR(funcIdx * sizeof(void*))
ir_ref FuncPtr = ir_LOAD_A(ir_ADD_A(
    ValidFT,
    ir_CONST_ADDR(static_cast<uintptr_t>(ResolvedFuncIdx) * sizeof(void *))));
```
Since `funcIdx` is always a compile-time constant, this is both correct
and simpler.

**Proper fix** (upstream):
`ir_match_fuse_addr_all_useges` should either skip instructions with
sub-4-byte types when walking address chains, or not assert on them.

---

## Bug 3: x86 address-fusion assertion with IR_VOID indirect call return

**Syndrome**:
Same assertion as Bug 2:
`ir_x86.dasc:1871: ir_match_fuse_addr_all_useges: Assertion '((insn->type) < IR_DOUBLE) && ir_type_size[insn->type] >= 4' failed`

**Reproducer** (before the workaround):
Same as Bug 2 (pulldown-cmark at O2), but only triggered when a
void-returning function is called through an indirect `CALL` with
`IR_VOID` return type in the prototype.

**Root cause**:
When a `CALL` instruction has `IR_VOID` return type and is followed by
other address-computation instructions, the O2 optimizer's address-fusion
pass encounters the void-typed CALL node during its operand walk.
`IR_VOID` has `ir_type_size = 0`, failing the `>= 4` check.

The old code always returned `IR_I64` from `jit_direct_or_host` (even
for void callees), so this never arose.

**Workaround** (in `ir_builder.cpp`):
Always declare `IR_I64` return type in the call prototype, even for
void-returning callees.  The unused `rax` value is harmless on x86-64:
```cpp
ir_type DirectRetType;
if (RetType == IR_FLOAT) {
    DirectRetType = IR_FLOAT;
} else if (RetType == IR_DOUBLE) {
    DirectRetType = IR_DOUBLE;
} else {
    DirectRetType = IR_I64;  // includes void — avoids IR_VOID assertion
}
```

**Proper fix** (upstream):
Same as Bug 2 — the address-fusion pass should handle or skip
`IR_VOID`-typed nodes gracefully.

---

## Bug 4: O2 IR_OPT_INLINE miscompilation with IF/THEN/ELSE call pattern

**Syndrome**:
Silent wrong results (no crash, no assertion).  Compression benchmarks
produce incorrect output — e.g., bz2 outputs `compressed length: 60647`
instead of `57507`; rust-compression brotli size is `168242` instead of
`167110`.

**Reproducer** (before the workaround):
An earlier iteration of the inlined call code used `ir_IF` / `ir_IF_TRUE`
/ `ir_IF_FALSE` / `ir_MERGE_2` / `ir_PHI_2` to branch on a null-check
of FuncPtr, with the fast path calling the JIT function directly and the
slow path calling `jit_host_call`.  This produced correct results at O0
and O1, but wrong results at O2 (`IR_OPT_INLINE` enabled).

**Root cause**:
The IR library's `IR_OPT_INLINE` pass (enabled at O2) miscompiles the
IF/THEN/ELSE pattern containing two indirect calls (one in each branch)
merged with a PHI node.  The exact mechanism is unknown (likely incorrect
alias analysis or code motion across the branches), but the effect is
that memory operations (stores to the shared args buffer, loads from
memory in the callee) produce wrong values.

**Workaround** (in `ir_builder.cpp`):
Eliminated the branching pattern entirely.  For local functions
(`funcIdx >= ImportFuncNum`), the FuncTable entry is always non-null
(only unreachable trap stubs are skipped during JIT compilation), so the
null check is unnecessary.  The code now does an unconditional direct
call.

**Proper fix** (upstream):
The `IR_OPT_INLINE` pass should correctly handle IF/THEN/ELSE patterns
with indirect calls and PHI merges.  Likely a bug in how the pass
handles memory effects of calls in diamond control flow.

---

## Bug 5: Empty-block cycle in ir_emit causes infinite loop at O0

**Syndrome**:
The `shootout-base64` sightglass kernel hangs indefinitely during JIT
**compilation** (never reaches execution) at O0.  The hang is in
`_ir_skip_empty_blocks()` (`ir_cfg.c:1296`), which loops forever through
a cycle of two empty blocks.  O1 and O2 are unaffected because optimizer
passes (SCCP, DCE) transform or eliminate the pattern before code
emission.

**Reproducer**:
```
WASMEDGE_SIGHTGLASS_KERNEL=shootout-base64 WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=0 \
./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

**Root cause**:
The Wasm module contains an empty infinite loop — the IR equivalent of
`(loop $L (br $L))` — which the WasmEdge IR builder correctly translates
to:
```
LOOP_BEGIN(entry_edge, back_edge)   // block 9
END                                  // block 9
BEGIN                                // block 10
LOOP_END                             // block 10  → successor = block 9
```

During code emission (`ir_emit.c`, the instruction-matching phase), each
block is checked for emptiness: if a block contains only a start and end
instruction (no computation), and its sole successor is not itself, it is
marked `IR_BB_EMPTY`.  The self-loop guard
(`ctx->cfg_edges[bb->successors] != b`) only catches the trivial case of
a block whose successor is itself.  It does **not** detect multi-block
cycles: block 9 points to block 10 (9 ≠ 10, passes the check), and
block 10 points back to block 9 (10 ≠ 9, also passes).  Both blocks are
marked `IR_BB_EMPTY`.

Later, when emitting an `IF` instruction whose true branch leads through
a chain of empty blocks into this cycle, `ir_get_true_false_blocks()`
calls `ir_skip_empty_target_blocks()` → `_ir_skip_empty_blocks()`, which
follows the successor chain: block 3 (empty) → block 8 (empty) → block 9
(empty) → block 10 (empty) → block 9 → … — infinite loop.

**Fix** (applied to `thirdparty/ir`):

1. **`ir_emit.c` (primary):** Before marking a block as `IR_BB_EMPTY`,
   follow its successor chain through already-marked-empty blocks.  If
   the chain leads back to the current block, skip the `IR_BB_EMPTY`
   marking.  The un-marked block retains its label in the emission phase
   and emits its `END`/`LOOP_END` as a jump, correctly generating the
   empty infinite loop in machine code (a `jmp` back to itself through
   the empty chain).

2. **`ir_cfg.c` (safety fallback):** Added an iteration counter to
   `_ir_skip_empty_blocks()` bounded by `cfg_blocks_count`.  If
   exceeded, asserts (debug builds) and returns the current block to
   prevent hangs from any unforeseen empty-block cycle.
