# Bug: rust-compression crash at IR JIT O1

## Status: FIXED (DESSA spill-slot aliasing — compile index 63)

The specific DESSA parallel copy bug in compile index 63 has been fixed. A
second, unrelated O1 bug exists in compile index 337 (see "Remaining issues").

## Summary

The `rust-compression` sightglass kernel hits an `unreachable` trap at IR JIT optimization level O1. The miscompiled function is compile_index 63. Func 63 at O1 produces wrong computational results that propagate through wasm linear memory, eventually causing a legitimate trap stub to be reached via a code path that correct execution would never take.

## Root cause: FIXED — lost-copy in DESSA parallel copy (spill-slot aliasing)

### The bug

BB4's outer loop PHIs:
```
d_78 = PHI(0, d_80)       R30, SPILL=0xcc
d_80 = PHI(1, d_156)      R31, SPILL=0xd0
d_156 = d_80 + 1           R58, SPILL=0xcc  ← SAME SLOT AS d_78!
```

On the BB4 back-edge (BB17→BB4), the DESSA parallel copy emits:
```asm
0xb91: mov 0xd0(%rsp),%edx    # edx = d_80
0xb98: mov %edx,0xcc(%rsp)    # d_78 ← d_80 (OVERWRITES d_156!)
0xb9f: mov 0xcc(%rsp),%edx    # edx = d_80 (was supposed to be d_156!)
0xba6: mov %edx,0xd0(%rsp)    # d_80 ← d_80 (NO CHANGE! BUG!)
```

The intended semantics is `d_80 ← d_156 = d_80+1`, but d_156's value at 0xcc is destroyed by the preceding write of d_78 ← d_80 to the same slot.

### Why it happens

`ir_dessa_parallel_copy()` in `ir_emit.c:713` tracks dependencies using vreg IDs (R30, R31, R58). It doesn't detect that R30 (d_78) and R58 (d_156) share spill slot 0xcc. So it thinks writing R30 (d_78) doesn't affect R58 (d_156), and emits the moves in the wrong order without a temporary.

### Effect

d_80 never increments (stuck at 1). The outer loop runs forever. The inner loop pointer d_97 advances by 4 bytes per "iteration", eventually reading past valid wasm memory → corrupted arg[5] = 0x410216d9 → func 462 takes wrong branch → hits trap stub func 487.

### Fix

Added a canonicalization pass in `ir_emit_dessa_moves()` (ir_emit.c) before calling `ir_dessa_parallel_copy()`. When a copy's `from` vreg shares a physical spill slot with a different copy's `to` vreg, the `from` ID is replaced with the `to` ID. This makes the physical dependency visible to the parallel copy algorithm, which then either reorders the moves or uses a temporary register to break the cycle.

After canonicalization, any copies that became no-ops (from == to) are removed.

### Verified by

1. BISECT=64 (func 63 at O1): passes after fix
2. All sightglass kernels at O2: all pass
3. The canonicalization correctly fires for func 63's BB4 back-edge (V58 → V30, spill slot 0xcc)

## Remaining issues

### Second O1 bug: compile index 337

After fixing func 63, full O1 still crashes. Bisection finds compile index 337:
- BISECT=337: passes (func 337 compiled at O2)
- BISECT=338: fails (func 337 compiled at O1)

This is a **different** bug — no DESSA spill-slot aliasing was detected in func 337's DESSA blocks. The crash call chain is similar (func 462 → func 466 → trap stub 487) but func 337, like func 63, is not on the stack — it produces wrong values through memory.

Key differences from the func 63 bug:
- O1 + bound_check=1 does NOT fix it (segfaults instead)
- No DESSA canonicalization is triggered for func 337
- Func 337 is large: 14487 bytes native code, 3722 lines IR, 1182 vregs

This bug is out of scope per CLAUDE.md (O1 is ignored for sightglass workflows).

## Files changed

- `thirdparty/ir/ir_emit.c` — DESSA spill-slot canonicalization in `ir_emit_dessa_moves()`
- `lib/vm/ir_jit_engine.cpp` — `jit_unreachable_trap` function + jmpVal==3 handler
- `include/vm/ir_jit_engine.h` — `jit_unreachable_trap` declaration
- `lib/executor/helper.cpp` — FuncTable routes uncompiled funcs to `jit_unreachable_trap`
- `lib/executor/instantiate/module.cpp` — Trap stub logging, comment improvements

## Bisection (original investigation)

The miscompiled function is **compile_index 63** (wasm func 75, since ImportFuncNum=12).

```shell
WASMEDGE_IR_JIT_BISECT=63 ...  # passes (func 63 compiled at O2)
WASMEDGE_IR_JIT_BISECT=64 ...  # fails  (func 63 compiled at O1)
```

## Module info

- 637 defined wasm functions, 12 imports (ImportFuncNum=12)
- 2 trap stubs: FuncIdx 487 (ci=475), FuncIdx 512 (ci=500)
- Func 63 IR: 890 lines (before), 1018 lines (O1 after), 866 lines (O2 after)
- Func 63 native code: 3908 bytes (O1), 3468 bytes (O2)
