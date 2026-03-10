---
name: IR JIT high-impact improvements
overview: |
  Three targeted improvements to the IR JIT path: fix float ABI (valVariantToRaw),
  avoid redundant work per call (module env cache + reusable args buffer), and
  use one engine instance for both compile and run (fix ODR, clear ownership).
  Scope is limited to these changes; existing limitations (table ops, memory.size/grow,
  float math placeholders) are documented and left for follow-up.
todos: []
isProject: false
---

# IR JIT high-impact improvements

## Summary

This plan describes **three high-impact, low-risk improvements** to the IR JIT pipeline. They address **correctness** (float arguments/returns), **performance** (less per-call work), and **structure** (single engine, no duplicate statics).

1. **Type-correct `valVariantToRaw` (F32/F64)**  
   The JIT passes arguments and returns as raw `uint64_t` slots. Today all types are converted with `Val.get<uint64_t>()`, which is wrong for F32/F64: float bit patterns must be copied with `memcpy`, not reinterpreted as integers. Fixing this makes float-using code (e.g. Sightglass shootout-matrix) receive and return correct values, and is a prerequisite for any float execution tests or future float math intrinsics.

2. **Cache FuncTable/GlobalBase per module and reuse args buffer**  
   On every JIT invoke we currently rebuild the function table and global pointers from the module instance, and allocate a fresh vector for marshalling arguments. For repeated invokes of the same module this is redundant. The plan adds a **per-module cache** (keyed by `ModuleInstance*`) holding FuncTable, GlobalPtrs, and MemoryBase, and a **reusable args buffer** inside the engine. Lookup is on the hot path; build only on cache miss. No new opcodes or IR changes—only reuse of data we already compute.

3. **Single IR JIT engine instance**  
   There are two separate `static IRJitEngine` definitions: one in the **instantiation** path (module.cpp) and one in the **execution** path (helper.cpp). That implies two different engine instances (and potential ODR issues). The plan moves ownership to the **Executor**: one `std::unique_ptr<IRJitEngine>` and a lazy `getIRJitEngine()` used by both instantiate and enterFunction. Compilation and execution then share the same engine; no behavioral change beyond fixing the duplicate static.

**What this plan does not do:** It does not add new opcode support (table ops, memory.size/grow), float math placeholders (sqrt, ceil, etc.), or bounds checking. Those are called out in “Current limitations” and left for follow-up. The three items above are independent of those gaps and safe to implement within the current IR JIT scope.

---

## Current limitations (from IR_JIT_IMPLEMENTATION.md)

Per [IR_JIT_IMPLEMENTATION.md](IR_JIT_IMPLEMENTATION.md):

- **Table ops:** Not implemented (`table.get`, `table.set`, `table.size`, `table.grow`, `table.fill`, `table.copy`, `table.init`, `elem.drop`). These opcodes are not lowered; if a function body contains them, build would fail and the function stays interpreter. Our plan does not add table op lowering; the **cache** only stores the **function table** (array of pointers for `call`/`call_indirect`), which is already built from the module instance. No change needed for cache when table ops are added later (table mutation could be handled by cache invalidation in a future change).
- **Memory ops:** Load/store are implemented and tested (19 execution tests). Bounds checking is currently skipped in the builder (`buildBoundsCheck` is a no-op). `memory.size` and `memory.grow` are not implemented. Our plan does not touch memory lowering; cache stores `MemoryBase` (pointer to linear memory) only. Safe to proceed.
- **Float:** F32/F64 binary (add/sub/mul/div/min/max), compare, and unary (abs, neg) are implemented in the builder. **Execution correctness for floats is not yet tested** (doc: "Float operations not yet tested"). F32/F64 math placeholders (sqrt, ceil, floor, trunc, nearest) return the operand unchanged. **valVariantToRaw** today uses `Val.get<uint64_t>()` for all types, which is wrong for F32/F64 (bit pattern must be preserved). Fixing it is a **prerequisite** for correct float execution and for any future float execution tests; it does not depend on completing float math placeholders.

**How we tackle these in this plan:**

1. **valVariantToRaw:** Fix F32/F64 so the JIT ABI is correct for floats. This enables existing F32/F64-using code (e.g. Sightglass shootout-matrix) to get correct arguments/returns and sets the basis for adding float execution tests and fixing float math placeholders later.
2. **Cache and args buffer:** Independent of table/memory/float completeness. Cache key is `ModuleInstance`*; we only cache what we already build (FuncTable, GlobalBase, MemoryBase). No new opcodes required.
3. **Single engine:** Purely structural; no interaction with incomplete opcodes.

After this plan, recommended follow-ups (out of scope here): add execution tests for F32/F64; implement float math intrinsics (sqrt/ceil/floor/trunc/nearest); add bounds checking and/or memory.size/grow when needed.

---

## 1. Type-correct valVariantToRaw

**Problem:** In [lib/vm/ir_jit_engine.cpp](lib/vm/ir_jit_engine.cpp), `valVariantToRaw` uses `Val.get<uint64_t>()` for all types. For F32/F64 the bit pattern must be preserved (e.g. via `memcpy`), not reinterpreted as integer, so float arguments can be wrong.

**Changes:**

- **Signature:** Add a `ValType` parameter so conversion is type-aware:
  - `uint64_t valVariantToRaw(const ValVariant &Val, ValType Type) const noexcept;`
- **Implementation:** Mirror the logic of existing `rawToValVariant` (lines 199–217):
  - `TypeCode::I32` → `return Val.get<uint32_t>();` (zero-extend to 64 bits if needed)
  - `TypeCode::I64` → `return Val.get<uint64_t>();`
  - `TypeCode::F32` → load `Val.get<float>()` into a `uint64_t` via `memcpy` (e.g. into the low 4 bytes or full 8), return it
  - `TypeCode::F64` → load `Val.get<double>()` into a `uint64_t` via `memcpy`, return it
  - Default → return 0
- **Call site:** In `invoke()` (lines 122–124), the loop already has `ParamTypes`; change to:
  - `ArgsRaw[i] = valVariantToRaw(Args[i], ParamTypes[i]);`

**Files:** [include/vm/ir_jit_engine.h](include/vm/ir_jit_engine.h) (declaration), [lib/vm/ir_jit_engine.cpp](lib/vm/ir_jit_engine.cpp) (definition and call site).

---

## 2. Caching FuncTable/GlobalBase and reusing args buffer

### 2.1 Cache FuncTable and GlobalBase per module

**Problem:** In [lib/executor/helper.cpp](lib/executor/helper.cpp) (lines 376–428), every IR JIT invoke rebuilds `FuncTableStorage` and `GlobalPtrStorage` from `ModInst` (loops over all functions and globals). This is redundant when the same module is invoked repeatedly.

**Approach:** Cache the built env (FuncTable + GlobalBase + MemoryBase) keyed by `ModuleInstance const`*. Rebuild only on cache miss; reuse on hit.

**Changes:**

- **Executor-owned cache:** Add a cache structure and map inside the Executor (so only the execution path that already has `this` uses it). Keep it under `#ifdef WASMEDGE_BUILD_IR_JIT`.
- **Cache struct:** Hold the storage that currently lives in `static thread_local` vectors, so the pointers remain valid:
  - `struct IRJitEnvCache { std::vector<void*> FuncTable; std::vector<ValVariant*> GlobalPtrs; void* MemoryBase = nullptr; };`
  - FuncTable = `FuncTable.data()`, GlobalBase = `GlobalPtrs.data()`, MemoryBase = first memory or nullptr.
- **Cache map:** e.g. `std::unordered_map<const Runtime::Instance::ModuleInstance*, IRJitEnvCache> IRJitEnvCache`_ (or a similar container). Key is the module instance pointer; no need to invalidate on module teardown for correctness (we never invoke after module is destroyed). Optionally document that cache entries are not removed when a module is unregistered (bounded by number of modules ever instantiated).
- **Lookup in enterFunction (helper.cpp):** When preparing to call `IREngine.invoke(...)`:
  - Look up `ModInst` in the cache.
  - If miss: build FuncTable and GlobalPtrs exactly as today (same loops), set MemoryBase, insert into cache.
  - If hit: use cached `FuncTable.data()`, `GlobalPtrs.data()`, `MemoryBase`.
- **Remove** the `static thread_local std::vector<void*> FuncTableStorage` and `static thread_local std::vector<ValVariant*> GlobalPtrStorage` from helper.cpp; their content is now inside the cache entry for the current `ModInst`.

**Files:** [include/executor/executor.h](include/executor/executor.h) (cache struct and map member under `#ifdef`), [lib/executor/helper.cpp](lib/executor/helper.cpp) (use cache in the IR JIT branch of `enterFunction`).

### 2.2 Reuse args buffer in IRJitEngine::invoke

**Problem:** In [lib/vm/ir_jit_engine.cpp](lib/vm/ir_jit_engine.cpp) (lines 121–124), each `invoke` allocates a new `std::vector<uint64_t> ArgsRaw(ParamTypes.size())`.

**Approach:** Keep a single reusable buffer inside the engine and resize per invoke.

**Changes:**

- **Member:** In [include/vm/ir_jit_engine.h](include/vm/ir_jit_engine.h), add under private: `mutable std::vector<uint64_t> ArgsBuffer_;`
- **In invoke():** Replace `std::vector<uint64_t> ArgsRaw(ParamTypes.size());` with:
  - `ArgsBuffer_.resize(ParamTypes.size());`
  - Use `ArgsBuffer_.data()` (or empty-check) where `ArgsData` is used; fill with `valVariantToRaw(Args[i], ParamTypes[i])` as in item 1.

**Files:** [include/vm/ir_jit_engine.h](include/vm/ir_jit_engine.h), [lib/vm/ir_jit_engine.cpp](lib/vm/ir_jit_engine.cpp).

---

## 3. Single IR JIT engine instance

**Problem:** There are two separate `static VM::IRJitEngine IREngine` definitions: one in [lib/executor/instantiate/module.cpp](lib/executor/instantiate/module.cpp) (line 145) and one in [lib/executor/helper.cpp](lib/executor/helper.cpp) (line 432). That gives two different engines (ODR violation in practice: two copies used in different TUs). Compilation uses one, execution uses the other.

**Approach:** Own a single `IRJitEngine` in the Executor and use it in both instantiation and execution.

**Changes:**

- **Executor member:** In [include/executor/executor.h](include/executor/executor.h), under `#ifdef WASMEDGE_BUILD_IR_JIT`:
  - Add `#include "vm/ir_jit_engine.h"` (or forward declare and include in .cpp only; if the header is included, add it near other optional includes).
  - In the private section of `Executor`, add: `mutable std::unique_ptr<VM::IRJitEngine> IRJitEngine_;`
  - Add a private accessor: `VM::IRJitEngine& getIRJitEngine() const;` that creates the engine on first use (if `IRJitEngine`_ is null, set it to `std::make_unique<VM::IRJitEngine>()`, then return `*IRJitEngine`_). This keeps construction lazy and avoids pulling in the implementation when IR JIT is disabled and the path is not used.
- **Instantiation:** In [lib/executor/instantiate/module.cpp](lib/executor/instantiate/module.cpp), inside the same `#ifdef WASMEDGE_BUILD_IR_JIT` block, replace `static VM::IRJitEngine IREngine` with use of the Executor’s engine: e.g. `auto &IREngine = getIRJitEngine();` (or `this->getIRJitEngine()`). All uses of `IREngine.compile(...)` stay the same.
- **Execution:** In [lib/executor/helper.cpp](lib/executor/helper.cpp), replace `static VM::IRJitEngine IREngine` with e.g. `auto &IREngine = getIRJitEngine();`. All uses of `IREngine.invoke(...)` stay the same.

**Files:** [include/executor/executor.h](include/executor/executor.h), [lib/executor/instantiate/module.cpp](lib/executor/instantiate/module.cpp), [lib/executor/helper.cpp](lib/executor/helper.cpp). If the getter is implemented in a .cpp file, add the implementation in a file that sees the full Executor definition (e.g. a new helper in executor or in one of the existing executor .cpp files that already includes the VM header). Checking the build: helper.cpp and module.cpp both include executor.h; if getIRJitEngine() is inline in the header (or defined in a .cpp that is linked), no extra file is strictly necessary. Prefer defining getIRJitEngine() in a single .cpp (e.g. executor.cpp or a small ir_jit_glue.cpp) to avoid defining it in both helper and module.

**Note:** Unit tests (e.g. [test/ir/ir_basic_test.cpp](test/ir/ir_basic_test.cpp), [test/ir/ir_execution_test.cpp](test/ir/ir_execution_test.cpp)) that instantiate their own `IRJitEngine` locally are unchanged; only the production path (Executor::instantiate and Executor::enterFunction) uses the shared engine.

---

## Order of implementation

1. **Type-correct valVariantToRaw** — Small, self-contained; fixes correctness and unblocks float-heavy workloads.
2. **Single IR JIT engine** — Removes duplicate static; clarifies ownership and avoids ODR issues.
3. **Cache + args buffer** — Cache is in Executor (same place as the engine); args buffer is inside the engine used by the (now single) invoke path.

## Testing

- Run the existing IR JIT test suites (e.g. `ctest -R IR`) and Sightglass benchmarks to ensure no regressions.
- Add or run a test that invokes a JIT-compiled function with F32/F64 parameters and checks results (to guard valVariantToRaw). This also starts addressing the "float execution not yet tested" gap from IR_JIT_IMPLEMENTATION.md.
- Optionally run a micro-benchmark that repeatedly invokes the same JIT function (same module) to observe improvement from caching and args-buffer reuse.
- Note: Kernels that use unsupported opcodes (table ops, memory.size/grow, etc.) may still skip or fail JIT for those functions; the plan does not extend opcode coverage.

