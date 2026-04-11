# IR JIT Call Design

All call-related logic lives in [lib/vm/ir_builder.cpp](../../lib/vm/ir_builder.cpp)
(`visitCall`, line ~2921), with trampolines in
[lib/executor/helper.cpp](../../lib/executor/helper.cpp) and the execution
environment defined in
[include/vm/ir_jit_engine.h](../../include/vm/ir_jit_engine.h).

## Uniform Calling Convention

Every JIT-compiled Wasm function is compiled to the same C signature:

```c
ret_type func(JitExecEnv *env, uint64_t *args)
```

- **`env`** (`JitExecEnv*`): carries all runtime context — function table,
  global base, memory base, trampoline pointers, memory size for bounds
  checking, tier-2 call counters.
- **`args`** (`uint64_t*`): flat buffer of Wasm parameters packed into 64-bit
  slots (i32 zero-extended, f32/f64 bit-reinterpreted via `memcpy`, refs
  occupy two consecutive slots).

The return type varies per function: `uint64_t`, `float`, `double`, or `void`.

## Shared Argument Buffer

At the top of every compiled function, a single stack allocation is emitted:

```c
SharedCallArgs = ir_ALLOCA(MaxCallArgs * 8)
```

`MaxCallArgs` is the maximum parameter count across all call sites in the
function. Every call reuses this buffer — no per-call allocation. Before each
call the compiler emits stores to pack Wasm operands into the buffer.

## Three Call Paths

The decision is made at compile time in `visitCall` (line 2968):

```c
bool IsHostCall = (Op == OpCode::Call && ResolvedFuncIdx < ImportFuncNum);
```

```
Wasm "call N"
  |
  +-- N < ImportFuncNum ----------> PATH 1: Host/Import Call
  |                                  WASI, env imports, cross-module
  |                                  -> jit_host_call trampoline
  |
  +-- N >= ImportFuncNum ----------> PATH 3: Direct Wasm-to-Wasm Call
                                     module-internal functions
                                     -> load FuncTable[N], call reg

Wasm "call_indirect T"
  |
  +-- tableIdx == 0 &&            > PATH 2a: Inline Fast Path (table 0)
  |   canonical ID available         bounds + type ID + null guards
  |                                  -> direct call via shadow dispatch table
  |                                  -> cold fallback to PATH 2b
  |
  +-- tableIdx != 0 or           > PATH 2b: Trampoline Slow Path
      no canonical ID                -> jit_call_indirect trampoline
```

---

### Path 1: Host/Import Call

**Condition:** `OpCode::Call` and `funcIdx < ImportFuncNum`

**What ends up here:** any function the module declared as an import, whose
body lives outside the module (typically in C++).

- WASI calls: `fd_write`, `fd_read`, `proc_exit`, `clock_time_get`, etc.
- Emscripten-style host callbacks: `env.abort`, `env.emscripten_memcpy_js`
- Cross-module linked functions (appear as imports from the caller's module)

**Compiled IR (ir_builder.cpp ~2970):**

```
pack args -> SharedCallArgs
call jit_host_call(env, funcIdx, SharedCallArgs) -> i64
unpack result (truncate i32, bitcast f32/f64)
```

**Trampoline (`jit_host_call`, helper.cpp ~51):**

1. Convert `uint64_t args[]` to `ValVariant[]`
2. **Fast path:** if the resolved target is itself a JIT function
   (`funcInst->isIRJitFunction()`), cast to the uniform signature and call
   directly — avoids interpreter overhead for cross-module Wasm-to-Wasm.
3. **Slow path:** dispatch through `Executor::jitCallFunction()` into the
   interpreter. Pushes a dummy stack frame to preserve the caller's module
   context (needed by WASI host functions that access memory).
4. On `proc_exit` (`Terminated`), `longjmp` back to `invoke()`.

---

### Path 2: Indirect Call (`call_indirect`)

**Condition:** `OpCode::Call_indirect` (any function index)

**What ends up here:** calls where the target is resolved at runtime via a
Wasm table index on the operand stack.

- C/C++ function pointers: `fn_ptr(a, b)` compiles to `call_indirect`
- C++ virtual dispatch: `obj->method()` through a vtable in a Wasm table
- Rust `dyn Trait` dispatch
- `qsort`-style comparator callbacks
- Any computed/dynamic dispatch

#### Path 2a: Inline Fast Path (table 0)

**Condition:** `tableIdx == 0` **and** `TypeIdx < CanonicalTypeIds.size()`
**and** `CanonicalTypeIds[TypeIdx] != 0`

For table 0 with a known canonical type ID, the hot path is fully inlined
into generated IR — no trampoline call in the common case. This replaces
the expensive `TypeMatcher::matchType` structural comparison with a single
integer compare via the **canonical type registry**.

**Shadow dispatch table:** A flat `DispatchEntry[]` array indexed by table
element index, stored in `JitExecEnv`. Each entry is 16 bytes:

```c
struct DispatchEntry {
  void *CodePtr;            // offset 0: JIT native func ptr (null if not JIT'd)
  uint32_t CanonicalTypeId; // offset 8: canonical type ID (0 = invalid)
  uint32_t _pad;            // offset 12
};
```

The table is built by `buildIRJitEnvCache()` (helper.cpp) from table 0's
elements, and maintained by five table-mutating trampolines (`jit_table_set`,
`jit_table_grow`, `jit_table_fill`, `jit_table_copy`, `jit_table_init`).
`jit_table_grow` conservatively sets `Table0DispatchSize = 0`, which forces
all subsequent `call_indirect` in that JIT invocation to take the slow path
until the next `invoke()` rebuilds the cache. The dispatch table pointer is
loaded fresh from `env` at each call site (not prologue-cached) so
invalidation is always visible.

**Canonical type registry:** A process-global singleton
(`include/vm/canonical_type_registry.h`) that maps structurally-equal
`FunctionType` → `uint32_t` ID. IDs are assigned at module instantiation
time (module.cpp) for every entry in the type section, and stored per-entry
in the shadow dispatch table.

**Compiled IR (ir_builder.cpp, `visitCallIndirect`):**

```
pack args -> SharedCallArgs
compute retTypeCode (0=void, 1=i32, 2=i64, 3=f32, 4=f64)

// --- Inline guard chain (all guards use ir_IF_FALSE_cold) ---
dispBase = LOAD_A(env + offsetof(JitExecEnv, Table0Dispatch))
dispSize = LOAD_U32(env + offsetof(JitExecEnv, Table0DispatchSize))

// Guard 1: bounds check (unsigned)
if (elemIdx >= dispSize) goto slow_path

// Load entry: base + zext(elemIdx) * 16
entry = dispBase + zext(elemIdx) * sizeof(DispatchEntry)
code_ptr = LOAD_A(entry + 0)
canon_id = LOAD_U32(entry + 8)

// Guard 2: type ID mismatch
if (canon_id != expectedCanonId) goto slow_path

// Guard 3: null code pointer (not JIT'd)
if (code_ptr == null) goto slow_path

// Fast path: direct call with typed return
fast_result = CALL code_ptr(env, SharedCallArgs)
// Normalize to i64 via alloca+store+load (for float/double)
fast_i64 = ...
goto done

slow_path:
  // Merge all 3 guard-failure branches
  slow_result = CALL jit_call_indirect(env, tableIdx, elemIdx,
                                       typeIdx, SharedCallArgs, retTypeCode)
  goto done

done:
  result = PHI(fast_i64, slow_result)  // both IR_I64
  unpack result (truncate i32, bitcast f32/f64)
```

On x86-64, the inline fast path compiles to ~5 instructions (2 loads, 2
compares, 1 indirect call) before hitting the target function — compared to
a full C++ trampoline call with interpreter dispatch on the slow path.

#### Path 2b: Trampoline Slow Path

**Condition:** `tableIdx != 0`, or no canonical type ID available, or any
inline guard failure (bounds, type ID mismatch, null code pointer).

This is the original trampoline path, now used as the cold fallback.

**Compiled IR (ir_builder.cpp):**

```
pack args -> SharedCallArgs
compute retTypeCode (0=void, 1=i32, 2=i64, 3=f32, 4=f64)
call jit_call_indirect(env, tableIdx, elemIdx, typeIdx,
                       SharedCallArgs, retTypeCode) -> i64
unpack result
```

**Trampoline (`jit_call_indirect`, helper.cpp ~193 -> `Executor::jitCallIndirect` ~584):**

1. Bounds-check `elemIdx` against `table.size()` (trap: `UndefinedElement`)
2. Load `table[elemIdx]`, null-check (trap: `UninitializedElement`)
3. Resolve to `FunctionInstance`, type-check against expected signature via
   `TypeMatcher::matchType` (trap: `IndirectCallTypeMismatch`)
4. **Fast path:** if target is JIT-compiled, dispatch directly with
   `retTypeCode`-based switch for correct return type handling.
5. **Slow path:** convert args to `ValVariant[]`, dispatch through interpreter.

---

### Path 3: Direct Wasm-to-Wasm Call

**Condition:** `OpCode::Call` and `funcIdx >= ImportFuncNum`

**What ends up here:** calls to functions defined within the same module,
where the target index is known statically.

- Ordinary function calls: `helper(x)`, `sort(arr, n)`
- Recursive calls: `factorial(n-1)`
- Standard library internals: `malloc` calling `sbrk`, `memcpy` helpers
- Any module-internal call the Wasm toolchain did not inline

This is the **vast majority** of calls in typical modules. A C module with
500 internal functions and 20 imports has ~96% of static call sites here.

**Compiled IR (ir_builder.cpp ~3069):**

```
load FuncTable[funcIdx] -> funcPtr
pack args -> SharedCallArgs
call funcPtr(env, SharedCallArgs) -> ret_type
```

The return type (`IR_I64`, `IR_FLOAT`, `IR_DOUBLE`) is set directly on the
IR `CALL` node, so the backend uses the correct register (RAX vs XMM0).
No trampoline, no type checks, no `ValVariant` conversion.

**x86-64 emission (ir_emit_x86.h):**

```asm
mov  rax, [FuncTable + i*8]   ; load function pointer
call rax                       ; indirect call (2 args in rdi, rsi)
```

Note: with PLT stubs (see [direct_call_doc.md](direct_call_doc.md)), the
indirect `call rax` is replaced by a direct `call rel32` to a stub, which
the branch predictor handles much better.

---

## Trap / Unwind Mechanism

The JIT does not use C++ exceptions. All traps use `setjmp`/`longjmp`:

- `invoke()` (`ir_jit_engine.cpp`) calls `setjmp(g_termination_buf)` before
  entering JIT code.
- Trampolines `longjmp` back with a code:
  - **1** -> Terminated (`proc_exit`)
  - **2** -> Memory out of bounds (`jit_oob_trap`)
  - **3** -> Unreachable (`jit_unreachable_trap`)

Unreachable trap stubs are placed in `FuncTable` for functions that weren't
compiled (e.g. pure `unreachable` bodies), preventing null-pointer
dereferences on direct calls.

## Null-Safe Fallback (`jit_direct_or_host`)

For edge cases where the compiler cannot statically determine whether a
function is local or imported, `jit_direct_or_host` (helper.cpp ~153)
checks if the function pointer is null. If null, it dispatches via
`jit_host_call`; otherwise calls directly with return-type dispatch.

## Runtime Entry Point (`IRJitEngine::invoke`)

1. Populate `JitExecEnv` with pointers to module's function table, globals,
   memory, trampoline functions, and tier-2 counters.
2. Convert `ValVariant` arguments to raw `uint64_t` buffer.
3. `setjmp(g_termination_buf)` for trap unwinding.
4. Cast native function pointer to the appropriate return-type signature and
   call: `nativeFunc(env, args)`.
5. On `longjmp`: map jump value to `ErrCode` (Terminated, MemoryOutOfBounds,
   Unreachable).

## Shadow Dispatch Table Lifecycle

The shadow dispatch table for table 0 is built and maintained as follows:

1. **Build** (`buildIRJitEnvCache`, helper.cpp): After constructing
   `FuncTable`, iterate table 0 elements. For each `RefVariant`, resolve to
   `FunctionInstance*`, look up canonical type ID, get native code pointer.
   Store `{CodePtr, CanonicalTypeId}` per entry in `Cache.Table0Dispatch`.

2. **Populate** (`invoke`, ir_jit_engine.cpp): Pass `Table0Dispatch.data()`
   and size into `JitExecEnv` at offsets 96 and 104.

3. **Maintain** (5 trampolines, helper.cpp): When table 0 is mutated at
   runtime, the shadow table is updated in sync:
   - `jit_table_set` — writes a single entry
   - `jit_table_fill` — fills a range
   - `jit_table_copy` — updates dst range from src refs
   - `jit_table_init` — updates range from elem segment refs
   - `jit_table_grow` — conservatively sets `Table0DispatchSize = 0`
     (rebuilt on next `invoke()`)

4. **Read** (JIT code): Each `call_indirect` site loads `Table0Dispatch`
   and `Table0DispatchSize` fresh from `env` (not prologue-cached), so
   invalidation by `jit_table_grow` is immediately visible.

## Thread-Local Runtime State

Trampolines access the WasmEdge executor through four `thread_local` globals
(helper.cpp ~26-29):

```c
static thread_local Executor       *g_jitExecutor;
static thread_local StackManager   *g_jitStackMgr;
static thread_local ModuleInstance  *g_jitModInst;
static thread_local MemoryInstance  *g_jitMemory0;
```

These are set by `invoke()` before entering JIT code and used by every
trampoline that needs to dispatch through the interpreter or access module
state.
