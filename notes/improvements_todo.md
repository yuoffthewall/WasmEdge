# IR JIT Correctness & Robustness Improvements TODO

## P0 — Critical (silent wrong results or security vulnerabilities)

### 1. ~~Memory Bounds Checking~~ ✅ DONE

Implemented as a CALL to `jit_bounds_check(env, end)` trampoline before every
memory load/store. The trampoline reads `MemorySizeBytes` directly from
`JitExecEnv` (always current, even after `memory.grow`) and longjmps back to
`invoke()` with `MemoryOutOfBounds` error on OOB.

Also added SIGABRT guard in `safeIrJitCompile` to catch IR optimizer assertions
(SCCP, GCM) gracefully — affected functions fall back to the interpreter.

**Known limitation**: 2/30 sightglass kernels (bz2, regex) are miscompiled at
O1+ due to IR library optimizer bugs exposed by the extra CALL nodes. They work
correctly at O0.

---

### 2. Signed Division Does Not Trap on div-by-zero or Overflow

**File**: `lib/vm/ir_builder.cpp:963-964, 1018-1019`

`i32.div_s` / `i64.div_s` emit raw `ir_DIV_I32` / `ir_DIV_I64` with no guard.
Wasm spec requires trapping on:
- Division by zero (`divisor == 0`)
- Signed overflow (`INT_MIN / -1`)

Same issue for `rem_s` (lines 973-974, 1027-1028).

**Fix**: Emit explicit checks before the division:
```
if (divisor == 0) trap;
if (dividend == INT_MIN && divisor == -1) trap;   // div_s only
```
Or route through trampoline functions that call back to the Wasm trap mechanism.

---

### 3. Unsigned div/rem Returns 0 Instead of Trapping

**File**: `lib/vm/ir_builder.cpp:33-44`

The `wasm_i32_div_u` / `wasm_i64_div_u` / `wasm_i32_rem_u` / `wasm_i64_rem_u`
helper functions return 0 when the divisor is zero. The Wasm spec requires a
trap, not a silent zero.

**Fix**: Replace `return b ? (a / b) : 0;` with a trap call (e.g. longjmp to
the termination buffer, or call an `__unreachable` trampoline).

---

## P1 — High (use-after-free, race conditions)

### 4. `MemoryBase` Stale After `memory.grow`

**File**: `lib/vm/ir_builder.cpp:197`

`MemoryBase` is loaded from `JitExecEnv` once in the function prologue. After
`jit_memory_grow`, the underlying buffer may be `realloc`'d to a different
address. Although `jit_memory_grow` updates `env->MemoryBase`, the JIT code
continues using the stale SSA value loaded at function entry.

**Fix**: After emitting a `memory.grow` call, reload `MemoryBase` from `EnvPtr`:
```cpp
MemoryBase = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, MemoryBase))));
```
Verify that the `memory.grow` handler in `visitMemory` does this.

---

### 5. SIGSEGV Guard Not Thread-Safe

**File**: `lib/vm/ir_jit_engine.cpp:34-69`

`compileGuardHandler` replaces the process-wide `SIGSEGV` handler with
`sigaction`. If two threads compile concurrently, the `sa_old` save/restore
will race: thread A installs handler, thread B installs (saving A's handler),
thread A restores original (removing B's handler).

**Fix**: Use a global mutex around `safeIrJitCompile`, or use
`sigaltstack` + per-thread `SA_ONSTACK` handling, or fix the underlying IR
library crashes so the guard is unnecessary.

---

### 6. `FuncTablePtr` / `FuncTableSize` Stale After `table.grow`

**File**: `lib/vm/ir_builder.cpp:194-195`

Similar to `MemoryBase`, these are loaded once in the prologue. If
`jit_table_grow` reallocates the table, the cached SSA values become stale.

**Fix**: Verify that `jit_call_indirect` and `jit_table_*` trampolines re-read
from `env` rather than relying on cached values. If any JIT-emitted code uses
`FuncTablePtr` directly for table access, reload after `table.grow`.

---

## P2 — Medium (spec non-compliance, resource leaks)

### 7. Multi-Value Returns Not Supported

**Files**: `include/vm/ir_jit_engine.h:107`, `lib/vm/ir_builder.cpp:166-171`

Only the first return type is used (`RetTypes[0]`). Wasm multi-value returns
(multiple return values) are silently dropped, violating the spec for any
function with >1 return values.

---

### 8. `setjmp`/`longjmp` Across C++ Code

**File**: `lib/vm/ir_jit_engine.cpp:191`

`setjmp`/`longjmp` in `invoke()` for termination handling will skip C++
destructors of any stack-allocated objects between the `setjmp` and the
`longjmp`. If JIT code calls back into C++ trampolines that hold RAII objects,
those will leak.

**Fix**: Use `sigsetjmp`/`siglongjmp` consistently, and audit that no C++
destructors are skipped on the longjmp path.

---

### 9. `_dbg_func_id` Not Thread-Safe

**File**: `lib/vm/ir_jit_engine.cpp:114`

`static int _dbg_func_id` is incremented non-atomically from potentially
multiple threads.

**Fix**: Use `std::atomic<int>`.

---

## P3 — Low (minor issues)

### 10. Silent Fallthrough for Unsupported Types

**File**: `lib/vm/ir_builder.cpp:230`

Local initialization uses `ir_CONST_I32(0)` for unrecognized types (e.g.,
`externref`, `funcref` beyond `IR_ADDR`). Silently produces incorrect IR.

**Fix**: Return an error for unsupported types instead of silently emitting I32.

---

### 11. `pop()` Returns `IR_UNUSED` on Empty Stack Without Error

**File**: `lib/vm/ir_builder.cpp:286-293`

Popping from an empty value stack silently returns `IR_UNUSED`. A corrupted or
fuzzer-generated module could trigger this, cascading into invalid IR that
crashes the backend.

**Fix**: `assert(!ValueStack.empty())` or return an error.

---

### 12. `allocateExecutable` is Dead Code

**File**: `lib/vm/ir_jit_engine.cpp:243-253`

Defined but never called — `ir_jit_compile` manages its own memory. Remove it.
