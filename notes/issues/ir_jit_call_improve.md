# IR JIT Call Path Improvements

Assessment of the current call design and concrete improvement opportunities.
See [call_doc.md](../design_docs/call_doc.md) for the full design description.

## Current State

The design uses a uniform `(JitExecEnv*, uint64_t*)` calling convention for
all JIT-compiled functions, a shared pre-allocated argument buffer per
function, and C++ trampolines for host and indirect calls. This was the right
tradeoff for a first-generation JIT: it simplified wiring, got everything
working correctly, and doesn't paint us into a corner.

The main costs are paid on every call site and are worth addressing now that
correctness is stable.

---

## 1. Arguments Passed via Memory Buffer

**Severity:** High (performance)
**Effort:** Large

### Problem

Every call — including direct Wasm-to-Wasm — marshals all arguments through
a `uint64_t[]` buffer in memory:

- N stores to pack args at the call site
- N loads to unpack them in the callee prologue
- The buffer is an optimization barrier: the IR backend cannot propagate
  constants, eliminate dead args, or reason about aliasing through it

For a hot inner loop calling a small helper (common in Wasm — think
`factorial`, `swap`, comparators), this overhead can exceed the function body.

### Suggestion

For direct Wasm-to-Wasm calls where both sides are JIT-compiled, generate
**native-ABI calls** with args in registers (RDI/RSI/RDX/RCX/R8/R9, XMM0-7
on x86-64). The `ir` backend already supports multi-parameter `IR_CALL` with
register allocation.

Keep the `(env, args)` convention only at trampoline boundaries (host calls,
indirect calls to unknown targets, entry from `invoke()`).

This is the single highest-impact optimization for call-heavy workloads.

---

## 2. Thread-Local Globals for Runtime Context

**Severity:** Medium (correctness, design)
**Effort:** Medium

### Problem

Trampolines access runtime state through four `thread_local` globals
(`helper.cpp:26-29`):

```c
static thread_local Executor       *g_jitExecutor;
static thread_local StackManager   *g_jitStackMgr;
static thread_local ModuleInstance  *g_jitModInst;
static thread_local MemoryInstance  *g_jitMemory0;
```

Downsides:
- TLS lookup on every trampoline entry (not free on x86-64; `fs:`-relative
  loads through GOT on shared libraries)
- Can't have two independent Wasm instances calling each other on the same
  thread without swapping globals
- Error-prone: if `invoke()` forgets to set/restore them, silent corruption

### Suggestion

Pack these into `JitExecEnv` (or add a `JitRuntimeContext*` field to it).
Trampolines already receive `env` as their first argument — they should pull
everything from it. Eliminates TLS overhead and makes the design re-entrant.

---

## 3. `longjmp`-Based Trap Handling

**Severity:** Medium (correctness)
**Effort:** Medium

### Problem

- `longjmp` skips C++ destructors and RAII cleanup on the unwind path.
- Single global `g_termination_buf` — nested JIT->host->JIT calls share it,
  so a trap in the inner call longjmps past the outer call's cleanup.
- `jit_call_indirect` (helper.cpp ~209) longjmps with value 1 (Terminated)
  for *any* error, losing the distinction between `UndefinedElement`,
  `UninitializedElement`, and `IndirectCallTypeMismatch`.

### Suggestion

**Short-term:** Store the error code in a `JitExecEnv` field before the
longjmp so `invoke()` can recover the real trap reason.

**Medium-term:** Use a per-call-depth `jmp_buf` stack so nested
JIT->host->JIT is safe.

**Long-term:** Platform-specific unwinding (signal handler + safe points)
or structured trap returns.

---

## 4. Duplicated Return-Type Dispatch

**Severity:** Low (maintenance)
**Effort:** Small

### Problem

The `retTypeCode` switch-case pattern is copy-pasted in three places:
- `jit_direct_or_host` (helper.cpp ~158)
- `jit_call_indirect` fast path (helper.cpp ~631)
- `jit_host_call` call_indirect fast path (helper.cpp ~80)

Each handles f32/f64/i32/i64/void identically. The `jit_host_call` path has
already diverged: it only distinguishes void vs non-void for the
call_indirect fast path, losing f32/f64 return correctness.

### Suggestion

Extract a single `jit_dispatch_native(env, funcPtr, args, retTypeCode)`
helper and call it from all three sites. If native-ABI calls are adopted
(issue 1), this pattern disappears for the direct path entirely.

---

## 5. `call_indirect` Fully Outlined

**Severity:** Medium (performance)
**Effort:** Medium

### Problem

Every `call_indirect` goes through the C++ trampoline, which does: table
bounds check -> null check -> type check -> JIT fast-path or interpreter
slow-path. That is a full C function call with `std::vector` allocation on
the interpreter path, even when the target turns out to be a JIT function
(the common case in real workloads).

### Suggestion

Inline the hot checks into generated code:

1. Bounds-check the table index (one compare + trap branch)
2. Load the function pointer from a parallel JIT function pointer table
3. Null-check (trap branch)
4. Call directly

Fall back to the trampoline only for the cold path (type mismatch, null
element, interpreter target). The common case — calling a known-signature
JIT function via table — should be ~5 instructions, not a trampoline call.

Type checking can be deferred to a guard stub or side table keyed by
`(typeIdx, funcIdx)` pairs.

---

## 6. No Inlining

**Severity:** Medium (performance)
**Effort:** Large

### Problem

Every Wasm function is an opaque call. For small leaf functions (getters,
trivial math, single-operation wrappers), the call overhead (pack args ->
indirect call -> unpack args -> body -> return -> unpack result) can exceed
the function body itself.

### Suggestion

Longer-term optimization. Prerequisite: native-ABI direct calls (issue 1).
Once callee IR is available at the caller's compile time, the `ir` backend
can inline small callees. Heuristic: inline leaf functions with <N IR nodes.
This is a large win for real-world Wasm but depends on the other
improvements landing first.

---

## 7. Multi-Value Returns Not Supported

**Severity:** Low (correctness)
**Effort:** Small

### Problem

The code only handles `RetTypes[0]`. Wasm multi-value returns (spec-legal,
used by some toolchains for struct returns) silently drop all but the first
return value.

### Suggestion

Add an assertion or trap in `visitCall` when `RetTypes.size() > 1` so it
fails loudly rather than producing wrong results silently. Full multi-value
support can be added later if needed.

---

## Priority Order

| # | Issue                          | Impact | Effort | Depends on |
|---|--------------------------------|--------|--------|------------|
| 1 | Args via memory buffer         | High   | Large  | --         |
| 5 | call_indirect fully outlined   | Medium | Medium | --         |
| 2 | Thread-local globals           | Medium | Medium | --         |
| 3 | longjmp trap handling          | Medium | Medium | --         |
| 4 | Duplicated retTypeCode         | Low    | Small  | --         |
| 7 | Multi-value assertion          | Low    | Small  | --         |
| 6 | Inlining                       | Medium | Large  | 1          |

Items 4 and 7 are quick wins. Item 1 is the biggest single performance
improvement. Items 2 and 3 are correctness/robustness improvements that
don't depend on anything else.
