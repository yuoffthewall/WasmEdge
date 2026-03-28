# Tiered Compilation for WasmEdge: A Sea-of-Nodes Baseline JIT Approach

This project aims to solve WasmEdge's cold start problem by integrating a proven, lightweight JIT compiler. It bridges the gap between interpretation and heavy AOT compilation, making WasmEdge more competitive for modern cloud-native workloads.

## Table of Contents

1. [Architecture & Design](#architecture--design)
2. [Build & Test & Debug](#build--test--debug)
3. [Testing Strategy](#testing-strategy)
4. [References](#references)
5. [Implementation Status](#implementation-status)

---

# Introduction
WebAssembly (Wasm) has evolved from a client-side browser technology into a high-performance, secure execution environment for cloud-native, serverless, and edge computing applications. To meet the stringent performance demands of these environments, Wasm runtimes must optimize for two often conflicting metrics: rapid startup latency (cold start) and peak execution throughput. Traditional execution models force a trade-off: interpreters offer instant startup but slow execution, while Ahead-of-Time (AOT) and optimizing Just-in-Time (JIT) compilers produce efficient native code at the cost of significant compilation delays and memory usage.

To resolve this dichotomy, modern high-performance engines (such as V8 and Wasmtime) employ tiered compilation. This strategy utilizes a lightweight "baseline" compiler to generate code quickly for immediate execution, followed by a heavy "optimizing" compiler to recompile hot functions for peak performance. This thesis explores the implementation of such a tiered architecture within WasmEdge, a leading cloud-native Wasm runtime. Specifically, it proposes integrating the dstogov/ir framework—a lightweight compilation infrastructure based on Sea-of-Nodes IR—as a new Tier-1 baseline JIT compiler. By combining dstogov/ir's rapid compilation capabilities with WasmEdge's existing LLVM-based optimization pipeline, this research aims to achieve a system that offers both low-latency startup and high-performance steady-state execution.

# Problem Statement
While WasmEdge demonstrates exceptional peak performance in computational workloads, its current architecture lacks a multi-tiered compilation strategy. WasmEdge relies heavily on LLVM for both AOT and JIT compilation. Although LLVM generates highly optimized machine code, its compilation pipeline is computationally expensive and memory-intensive, which is often prohibitive for resource-constrained edge devices or short-lived serverless functions.

Consequently, WasmEdge faces a significant "cold start" problem, where applications must endure long pauses while LLVM compiles the code. Currently, WasmEdge lacks a lightweight baseline JIT implementation capable of bridging the gap between slow interpretation and expensive optimization. The lack of a fast-path compiler prevents the runtime from adapting dynamically to workload behavior, limiting its efficiency in scenarios where rapid responsiveness is as critical as raw throughput. This project addresses this deficiency by integrating `dstogov/ir` to establish a baseline compilation tier, thereby enabling a complete tiered JIT infrastructure.

# Background

## 1. Background: WebAssembly Compilation Evolution
`Wasm` is designed for near-native performance.
Three Compilation Strategies in `Wasm`:

- **Interpreter:** Fast startup, slow execution (good for short tasks).

- **AOT (Ahead-of-Time):** Best performance, but complex deployment and no dynamic adaptation.

- **JIT (Just-in-Time):** Dynamic compilation. Modern runtimes (V8, Wasmtime) use a Tiered JIT approach: starts with a fast/simple compiler (Baseline) for quick startup, then switches to a slower/complex compiler (Optimizing) for peak performance.

Current State: WasmEdge currently relies only on LLVM (AOT/Optimizing JIT) and lacks a lightweight baseline tier.

## 2. Theoretical Basis: Sea-of-Nodes IR
- **Concept:** A compilation representation where data dependencies and control flow are unified in a single graph of nodes, unlike traditional methods that separate them into blocks.

- **Advantages:** Easier to optimize (Global Code Motion), simplifies control flow handling (if/loop), and is friendly to incremental compilation.

## 3. The Framework: dstogov/ir
A lightweight C-based JIT framework implementing Sea-of-Nodes.

- **Features:** Built-in constant folding, code motion (GCM), and register allocation. It generates machine code directly (DynASM) without an external assembler.

- **Performance:** Compiles ~40x faster than GCC-O2, while generated code is only ~5% slower. It balances speed and quality perfectly for a baseline JIT.

## 4. Why Lightweight JIT? (Lessons from PHP)
- **Problem**: Heavy JITs like LLVM are too slow and memory-hungry for dynamic languages or short tasks (e.g., PHP requests).

- **Design Goals:** The dstogov/ir framework was built to be Simple, Compact, and Fast.

- **Result:** Used in PHP 8.4+, proving that a lightweight JIT can provide huge speedups with minimal overhead compared to LLVM.

## 5. Core Design Principles of dstogov/ir
- **Structure:** Uses a compact 16-byte struct for IR nodes to save memory.

- **Pipeline:** Construction -> Optimization -> Scheduling -> Register Allocation -> Code Gen.

- **JIT Features:** Supports "Guard" nodes (for speculative optimization) and snapshots (for restoring state if optimization fails), which are essential for tiered compilation.

## 6. Tiered Compilation Strategy & Integration
- **PHP Experience:** Uses a 2-tier model. Tier 1 (IR JIT) compiles everything quickly; Tier 2 (LLVM) recompiles only "hot" functions (executed frequently).

- **Relevance to Wasm:** Wasm is statically typed and simpler than PHP, making it an even better candidate for this integration.

- **Risks:** The framework is relatively new (mostly used in PHP).

- **Mitigation:** Implement progressively, keep LLVM as a fallback, and add rigorous testing for Wasm-specific features.

## 7. Reference Case: rv32emu
- **Overview:** A RISC-V simulator that successfully uses a 3-layer architecture (Interpreter -> Tier 1 JIT -> Tier 2 LLVM).

- **Key Takeaway:** "Progressive Investment"—only spend time compiling code that runs often. It proved that a tiered approach saves massive amounts of memory (60% less) while maintaining speed.

## 8. Problem Statement & Motivation
- **Issue:** WasmEdge uses LLVM for everything. This causes slow cold starts (hundreds of milliseconds) and high memory usage (50-100MB) for simple functions.

- **Solution:** Integrate dstogov/ir as a Tier-1 compiler.

- **Goal:** Achieve fast startup (via IR) and peak performance (via LLVM) in a single runtime, ideal for serverless and edge computing.

## 9. Architecture & Technical Plan
Workflow:

1. Wasm Bytecode enters WasmEdge.

2. **Tier 1 (implemented):** At instantiation, eligible functions are lowered with dstogov/ir and executed as native code (`JitExecEnv` ABI).

3. **Profiling:** Not implemented — there is no hotness counter driving recompilation in this tree.

4. **Tier 2 (not implemented):** LLVM recompilation of hot functions.

5. **Switch (not implemented):** Atomically replace the entry point when Tier 2 code is ready.

## 10. Implementation Details (as implemented in-tree)
- **Frontend:** `WasmToIRBuilder` walks `AST::Instruction` streams and emits dstogov/ir nodes (stack simulation, PHIs for control merge, `JitExecEnv` loads for memory/globals/tables).

- **Memory:** `IRJitEngine` uses **mmap** RW then **RX** (`mprotect`) for emitted code.

- **ABI:** Generated functions use **`RetType (*)(JitExecEnv *, uint64_t *)`**. `IRJitEngine::invoke` packs `ValVariant` args into `uint64_t[]`, fills `JitExecEnv`, and dispatches. Imports and complex calls use **C trampolines** (`jit_host_call`, `jit_call_indirect`, `jit_memory_grow`, table/bulk helpers, etc.) in `lib/executor/helper.cpp`.

## 11. Challenges & Future Work
- **Challenges:** Handling Wasm exceptions, atomic operations, and ensuring thread safety during the code swap.

- **Future Directions:** Adding speculative optimizations (guessing branch directions), Profile-Guided Optimization (PGO), and potentially hardware-specific acceleration.

---

# Architecture & Design

This section follows one path through the implementation: **where** IR JIT sits in WasmEdge, **which files** to open, **main types**, **runtime** behavior (including how compile-time and dispatch relate), **lowering** (visitor + macros + codegen choices), and a **short recap** table. It complements the thesis outline in [§9–§10](#9-architecture--technical-plan).

**Roadmap:** (1) position and unified compile/call flow → (2) source layout → (3) major components → (4) runtime and dispatch diagram → (5) instruction mapping and codegen choices → (6) recap.

## 1. Position in WasmEdge and end-to-end flow

The IR JIT is wired into **module instantiation** and **execution**: after load and validation, eligible defined functions are lowered and compiled; calls dispatch through the executor like other backends.

**Where IR JIT runs in the engine**

```
┌─────────────────────────────────────────────────────────────────┐
│ 1. Module Loading (Loader::parseModule)                         │
│    └── Parse .wasm binary into AST::Module                      │
├─────────────────────────────────────────────────────────────────┤
│ 2. Validation (Validator::validate)                             │
│    └── Type checking and semantic validation                    │
├─────────────────────────────────────────────────────────────────┤
│ 3. Instantiation (Executor::registerModule)                     │
│    └── Instantiate functions, memories, globals, tables         │
│    └── **IR JIT compilation** (after memory/tables/data init)   │
│        ├── Skip: `ForceInterpreter`, LLVM AOT module, or body   │
│        │   starting with `unreachable`                          │
│        ├── Per defined function: set module func types, types   │
│        │   section, globals, import count                      │
│        └── `IRJitEngine::compile` → `upgradeToIRJit`           │
│            (`IRGraph` argument currently `nullptr`)               │
├─────────────────────────────────────────────────────────────────┤
│ 4. Execution (`enterFunction` / `invoke`)                        │
│    └── If `isIRJitFunction()`: `IRJitEngine::invoke` + trampolines │
│    └── Else: interpreter or LLVM AOT path as usual               │
└─────────────────────────────────────────────────────────────────┘
```

**Unified compile-time vs run-time flow** (single view; details in §3–§4):

```
Instantiation (per eligible defined function):
  AST::Instruction[] → WasmToIRBuilder → ir_ctx → IRJitEngine (ir_check → ir_jit_compile)
        → native code (mmap, W^X via `mprotect`)
        → upgradeToIRJit(native, CodeSize, IRGraph) on FunctionInstance

Execution (each call to a JIT function):
  Executor → IRJitEngine::invoke(native, FuncType, Args, Rets, FuncTable, GlobalBase, MemoryBase, …)
        → generated: RetType (*)(JitExecEnv *, uint64_t *)
```

**Key integration points**

**Function upgrade** (`include/runtime/instance/function.h`)

```cpp
// After JIT compilation succeeds, upgrade function variant
bool upgradeToIRJit(void *NativeFunc, size_t CodeSize, ir_ctx *IRGraph);
```

**Compilation hook** (`lib/executor/instantiate/module.cpp`)

```cpp
// Compile functions after all module sections are instantiated
#ifdef WASMEDGE_BUILD_IR_JIT
  VM::IRJitEngine IREngine;
  VM::WasmToIRBuilder IRBuilder;
  // ... compile each function
  FuncInst->upgradeToIRJit(CompRes->NativeFunc, CompRes->CodeSize, nullptr);
#endif
```

(`IRGraph` is not retained after compile today.)

Caches, trampolines, and **`IRJitEnvCache`** are described once in **§4** (not repeated here). Enable the feature with CMake: [Build & Test & Debug](#build--test--debug) (e.g. `-DWASMEDGE_BUILD_IR_JIT=ON`). Loader-level coverage of this path is in [End-to-end loader tests](#end-to-end-loader-tests) under [Testing Strategy](#testing-strategy).

---

## 2. Source layout and code scale

### File structure

```
WasmEdge/
├── CMakeLists.txt                          [modified]
├── include/
│   ├── runtime/instance/
│   │   └── function.h                      [modified - upgradeToIRJit]
│   └── vm/
│       ├── ir_builder.h                    [new]
│       └── ir_jit_engine.h                 [new]
├── lib/
│   ├── executor/
│   │   ├── CMakeLists.txt                  [modified - IR JIT linkage]
│   │   ├── helper.cpp                      [modified - func_table, global/memory base]
│   │   └── instantiate/
│   │       └── module.cpp                  [modified - IR JIT compilation hook]
│   └── vm/
│       ├── CMakeLists.txt                  [modified]
│       ├── ir_builder.cpp                  [new]
│       └── ir_jit_engine.cpp               [new]
├── thirdparty/
│   ├── CMakeLists.txt                      [modified]
│   └── ir/
│       └── CMakeLists.txt                  [new]
├── test/ir/
│   ├── testdata/
│   │   ├── factorial.wat                   [new - E2E test module]
│   │   ├── factorial.wasm                  [new - compiled E2E test]
│   │   ├── fibonacci.wat                   [new - benchmark algorithms]
│   │   ├── fibonacci.wasm                  [new - compiled benchmarks]
│   │   └── sightglass/                     [Sightglass .wasm kernels]
│   ├── ir_e2e_test.cpp                     [new - E2E integration tests]
│   ├── ir_benchmark_test.cpp               [new - algorithm + Sightglass benchmarks]
│   └── run_sightglass_all.sh               [run Sightglass kernels: Interpreter/JIT/AOT]
└── thirdparty/ir/                           [dstogov/ir submodule]
    ├── ir.h
    ├── ir_builder.h
    └── examples/                           [studied for patterns]
```

### Code statistics (approximate; from current tree)

Core IR JIT (line counts drift over time):

| Component | ~Lines | Description |
|-----------|--------|-------------|
| `include/vm/ir_builder.h` | ~278 | Builder API, `LabelInfo`, env/ref helpers |
| `lib/vm/ir_builder.cpp` | ~2940 | Wasm→IR lowering |
| `include/vm/ir_jit_engine.h` | ~200 | `JitExecEnv`, trampolines, `IRJitEngine` |
| `lib/vm/ir_jit_engine.cpp` | ~337 | `compile`, `invoke`, mmap, `ir_check` |
| `ir_basic_test.cpp` | ~915 | 33 tests |
| `ir_instruction_test.cpp` | ~1237 | 58 tests |
| `ir_execution_test.cpp` | ~2093 | 81 tests |
| `ir_integration_test.cpp` | ~402 | 6 tests |
| `ir_e2e_test.cpp` | ~324 | 5 tests |
| `ir_benchmark_test.cpp` | ~1670 | 13 tests |

Executor / runtime: `lib/executor/instantiate/module.cpp` (IR JIT pass), `lib/executor/helper.cpp` (`jit_*`, `enterFunction` IR path, `jit_call_indirect`), `include/runtime/instance/function.h`, `include/executor/executor.h` (`getIRJitEngine`), CMake under `lib/vm`, `lib/executor`, `test/ir`, `thirdparty/CMakeLists.txt`.

---

## 3. Major components

### 3.1 `WasmToIRBuilder`
**Location**: `include/vm/ir_builder.h`, `lib/vm/ir_builder.cpp` (~2900 lines)

Translates a single function’s `AST::Instruction` stream into dstogov/ir. Module context is injected **after** `initialize()` (because `reset()` clears it), matching `lib/executor/instantiate/module.cpp`.

**Public API (representative)**:
```cpp
class WasmToIRBuilder {
public:
  Expect<void> initialize(const AST::FunctionType &FuncType,
                        Span<const std::pair<uint32_t, ValType>> Locals);
  void setModuleFunctions(Span<const AST::FunctionType *> FuncTypes) noexcept;
  void setModuleTypes(Span<const AST::FunctionType *> Types) noexcept;
  void setModuleGlobals(Span<const ValType> GlobalTypes) noexcept;
  void setImportFuncNum(uint32_t Num) noexcept;
  void setMaxCallArgs(uint32_t N) noexcept;
  Expect<void> buildFromInstructions(Span<const AST::Instruction> Instrs);
  ir_ctx *getIRContext() noexcept;
  void reset() noexcept;
private:
  // Visitors include visitCall, visitRefType, visitTable; control helpers for PHI/merge;
  // `LabelInfo` tracks loop back-edges, if/else termination, ref-typed results (two stack slots).
  ir_ctx Ctx;
  std::vector<ir_ref> ValueStack;
  std::map<uint32_t, ir_ref> Locals;
  std::map<uint32_t, ir_type> LocalTypes;
  std::vector<LabelInfo> LabelStack;
  // Cached ir_ref loads from JitExecEnv: FuncTablePtr, GlobalBasePtr, MemoryBase, helpers, ArgsPtr, ...
};
```

### 3.2 `IRJitEngine` and `JitExecEnv`
**Location**: `include/vm/ir_jit_engine.h`, `lib/vm/ir_jit_engine.cpp`

```cpp
struct JitExecEnv {
  void **FuncTable;
  uint32_t FuncTableSize;
  // ...
  void *GlobalBase;    // ValVariant*[] as void*
  void *MemoryBase;
  void *HostCallFn;         // jit_host_call
  void *DirectOrHostFn;    // jit_direct_or_host
  void *MemoryGrowFn;    // jit_memory_grow
  void *MemorySizeFn;    // jit_memory_size
  void *CallIndirectFn;  // jit_call_indirect
  uint64_t MemorySizeBytes;
  uint64_t RefResultBuf[2];
};

class IRJitEngine {
public:
  struct CompileResult { void *NativeFunc; size_t CodeSize; ir_ctx *IRGraph; };
  Expect<CompileResult> compile(ir_ctx *Ctx);
  Expect<void> invoke(void *NativeFunc, const AST::FunctionType &FuncType,
                      Span<const ValVariant> Args, Span<ValVariant> Rets,
                      void **FuncTable = nullptr, uint32_t FuncTableSize = 0,
                      void *GlobalBase = nullptr,
                      void *MemoryBase = nullptr, uint64_t MemorySize = 0);
  void release(void *NativeFunc, size_t CodeSize) noexcept;
  void releaseIRGraph(ir_ctx *Ctx) noexcept;
};
```

Native entry point shape: **`RetType (*)(JitExecEnv *, uint64_t *)`** (see `invoke` and `jit_call_indirect` fast paths in `helper.cpp`).

### 3.3 `FunctionInstance` extension
**Location**: `include/runtime/instance/function.h`

Extended to store IR JIT compiled functions.

```cpp
#ifdef WASMEDGE_BUILD_IR_JIT
struct IRJitFunction {
  void *NativeFunc;      // Native code pointer
  size_t CodeSize;       // For memory release
  ir_ctx *IRGraph;       // For tier-up to LLVM
};

// Data variant now includes IRJitFunction
std::variant<WasmFunction, Symbol<CompiledFunction>,
             std::unique_ptr<HostFunctionBase>, 
             IRJitFunction> Data;
#endif
```

---

## 4. Runtime behavior (executor, cache, trampolines, compile vs dispatch)

### 4.1 Mixed function table (`IRJitEnvCache` in `helper.cpp`).
  At runtime the cached per-module table is **not** “JIT-only”: each index holds the **native entry** for an IR JIT function, a **pointer to an LLVM `CompiledFunction`** symbol, or **`nullptr`** for wasm bodies still executed by the interpreter or for host functions. That allows `call`/`call_indirect` to reach the right target kind from generated code. When entering an IR JIT function, this cache is populated together with globals and memory0; **`IRJitEngine::invoke`** receives `FuncTable`, `GlobalBase`, `MemoryBase`, and the current memory size in bytes.

### 4.2 Thread-local trampoline context.
  `jit_host_call`, `jit_call_indirect`, and related paths use **thread-local** pointers to the active **`Executor`**, **`StackManager`**, and **`ModuleInstance`** (and memory0) while the JIT region runs. Trampolines assume they execute on the **same thread** as the executor invocation that invoked the JIT code.

### 4.3 Direct vs import vs indirect calls. 
  - **`jit_host_call`** — dispatches imports and interpreter paths through the executor. For **`call_indirect`**, the **function index** is encoded with **bit 31 set**: pass **`(0x80000000 | tableSlot)`** as `funcIdx` so the trampoline can resolve the table slot (see `ir_jit_engine.h` comments).  
  - **`jit_direct_or_host`** — null-safe **direct** `call`: if the callee pointer is null, falls back to `jit_host_call`-style dispatch.  
  - **`jit_call_indirect`** — full table resolution, type check, then JIT fast path or interpreter.

### 4.4 Traps and non-local returns.
  `IRJitEngine::invoke` installs **`setjmp`** on a buffer from **`wasmedge_ir_jit_get_termination_buf`**. **`jit_oob_trap`** uses **`longjmp`** with value **2** → **`MemoryOutOfBounds`**. Other non-zero returns (e.g. **`Terminated`** from `proc_exit` via **`jit_host_call`**) unwind without returning into generated code. This keeps trap semantics aligned with the interpreter.

### 4.5 Moving linear memory. 
  **`jit_memory_grow`** updates **`env->MemoryBase`** when the engine’s memory buffer is reallocated; generated code must always load the base from **`JitExecEnv`**, not cache a stale pointer across grow.

### 4.6 Optional env-driven checks. 
  **`jit_bounds_check`** is an outlined helper for linear-memory bounds; **in-IR** checks can be enabled with **`WASMEDGE_IR_JIT_BOUND_CHECK`** (see `WasmToIRBuilder::buildBoundsCheck`).


The diagram below is the **same instantiate vs invoke split** as §1, zoomed into how IR JIT sits next to host and LLVM paths inside the executor.

```
┌──────────────────────────────────────────────────────────────────┐
│                         WasmEdge Runtime                         │
├──────────────────────────────────────────────────────────────────┤
│  Executor (per call site)                                        │
│                                                                  │
│      ┌──────────────────┐                                        │
│      │ Host functions   │                                        │
│      └──────────────────┘                                        │
│      ┌──────────────────┐                                        │
│      │ LLVM AOT /       │  native symbol path                    │
│      │ CompiledFunction │                                        │
│      └──────────────────┘                                        │
│      ┌──────────────────┐                                        │
│      │ IR JIT           │  FuncInstance → native ptr             │
│      └────────┬─────────┘                                        │
│               │                                                  │
│               │  ┌─ Instantiate (per wasm func) ─────────┐       │
│               │  │                                       │       │
│               │  │ WasmToIRBuilder::buildFromInstructions()│     │
│               │  │            │                          │       │
│               │  │            ▼                          │       │
│               │  │        ir_ctx (dstogov/ir graph)      │       │
│               │  │            │                          │       │
│               │  │   IRJitEngine::compile()              │       │
│               │  │            │                          │       │
│               │  │            ▼                          │       │
│               │  │   ir_jit_compile()  [dstogov/ir]      │       │
│               │  │            │                          │       │
│               │  │   upgradeToIRJit(native,size,IRGraph) │       │
│               │  │                                       │       │
│               │  └───────────────────────────────────────┘       │
│               │                                                  │
│               │  ┌─ Run-time (each call) ────────────────┐       │
│               │  │                                       │       │
│               │  │ IRJitEngine::invoke(                  │       │
│               │  │   native, FuncType, Args, Rets,       │       │
│               │  │   FuncTable, GlobalBase, MemoryBase)  │       │
│               │  │            │                          │       │
│               │  │            ▼                          │       │
│               │  │ generated (JitExecEnv*, uint64_t*)    │       │
│               │  │                                       │       │
│               │  └───────────────────────────────────────┘       │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

---

## 5. Lowering: instruction mapping and codegen choices

The dstogov/ir framework uses a **Sea-of-Nodes** intermediate representation, where each instruction is a node and dependencies are edges. The `WasmToIRBuilder` class translates WebAssembly instructions to this IR graph using a visitor pattern.

### Sea-of-Nodes Representation

From the dstogov/ir documentation:
> "This representation unifies data and control dependencies into a single graph, where each instruction is represented as a Node and each dependency as an Edge between Nodes."

When you use IR macros, you're building this graph automatically:

```cpp
ir_ref a = ir_CONST_I32(10);      // Node: constant 10
ir_ref b = ir_CONST_I32(20);      // Node: constant 20  
ir_ref sum = ir_ADD_I32(a, b);    // Node: ADD with edges to 'a' and 'b'
ir_RETURN(sum);                    // Node: RETURN with edge to 'sum'
```

### Mapping Process Overview

The mapping follows this pattern:

```
┌──────────────────┐     ┌──────────────────┐     ┌──────────────────┐
│  WebAssembly     │     │   Dispatch in    │     │   IR Macros      │
│  Instruction     │ ──▶ │ visitInstruction │ ──▶ │   Generate       │
│  (OpCode)        │     │                  │     │   IR Nodes       │
└──────────────────┘     └──────────────────┘     └──────────────────┘
```

### Step 1: Main Dispatch (`visitInstruction`)

The entry point dispatches WebAssembly opcodes to category-specific handlers:

```cpp
Expect<void> WasmToIRBuilder::visitInstruction(const AST::Instruction &Instr) {
  OpCode Op = Instr.getOpCode();

  switch (Op) {
  // Constants
  case OpCode::I32__const:
  case OpCode::I64__const:
    return visitConst(Instr);

  // Binary operations
  case OpCode::I32__add:
  case OpCode::I32__sub:
  case OpCode::I32__mul:
    return visitBinary(Op);
    
  // Comparisons
  case OpCode::I32__eq:
  case OpCode::I32__lt_s:
    return visitCompare(Op);
    
    // ... more categories
    // call / call_indirect → visitCall; ref.* → visitRefType; table.* → visitTable
  }
}
```

### Step 2: Handler functions

Each category has a dedicated handler that uses IR macros. Here's how binary operations are mapped:

```cpp
Expect<void> WasmToIRBuilder::visitBinary(OpCode Op) {
  ir_ctx *ctx = &Ctx;           // IR macros need 'ctx' pointer
  ir_ref Right = pop();         // Pop operands from value stack
  ir_ref Left = pop();
  ir_ref Result = IR_UNUSED;

  switch (Op) {
  case OpCode::I32__add:
    Result = ir_ADD_I32(Left, Right);    // Map to IR ADD node
    break;
  case OpCode::I32__sub:
    Result = ir_SUB_I32(Left, Right);    // Map to IR SUB node
    break;
  case OpCode::I32__mul:
    Result = ir_MUL_I32(Left, Right);    // Map to IR MUL node
    break;
  case OpCode::I32__div_s:
    Result = ir_DIV_I32(Left, Right);    // Signed division
    break;
  case OpCode::I32__div_u:
    Result = ir_DIV_U32(Left, Right);    // Unsigned division
    break;
  // ... more operations
  }

  push(Result);                  // Push result back to value stack
  return {};
}
```

### Step 3: IR Macro Expansion

The IR macros (from `ir/ir_builder.h`) generate nodes in the Sea-of-Nodes graph:

```cpp
// Macro definition pattern from dstogov/ir
#define ir_ADD_I32(_op1, _op2)  ir_BINARY_OP_I32(IR_ADD, (_op1), (_op2))

// Which expands to:
ir_fold2(ctx, IR_OPT(IR_ADD, IR_I32), op1, op2)
```

#### Available IR Macro Categories

| Category | Example Macros | WebAssembly Mapping |
|----------|---------------|---------------------|
| **Constants** | `ir_CONST_I32()`, `ir_CONST_F64()` | i32.const, f64.const |
| **Arithmetic** | `ir_ADD_I32()`, `ir_MUL_I64()`, `ir_DIV_F()` | i32.add, i64.mul, f32.div |
| **Bitwise** | `ir_AND_I32()`, `ir_SHL_I64()`, `ir_ROL_I32()` | i32.and, i64.shl, i32.rotl |
| **Comparison** | `ir_EQ()`, `ir_LT()`, `ir_ULT()` | i32.eq, i32.lt_s, i32.lt_u |
| **Unary** | `ir_NEG_F()`, `ir_CTLZ_I32()` | f32.neg, i32.clz |
| **Conversion** | `ir_TRUNC_I32()`, `ir_SEXT_I64()`, `ir_FP2I32()`, `ir_INT2F()`, `ir_BITCAST_F()` | i32.wrap_i64, i64.extend_i32_s, i32.trunc_f32_s, f32.convert_i32_s, f32.reinterpret_i32 |
| **Memory** | `ir_LOAD_I32()`, `ir_LOAD_U8()`, `ir_STORE()`, `ir_ZEXT_A()`, `ir_ADD_A()` | i32.load, i32.load8_u, i32.store, address computation |
| **Control** | `ir_RETURN()`, `ir_IF()`, `ir_COND()` | return, if, select |

### Type Conversion Helper

WebAssembly types are mapped to IR types:

```cpp
ir_type WasmToIRBuilder::wasmTypeToIRType(ValType Type) const noexcept {
  auto Code = Type.getCode();
  if (Code == TypeCode::I32)      return IR_I32;
  else if (Code == TypeCode::I64) return IR_I64;
  else if (Code == TypeCode::F32) return IR_FLOAT;
  else if (Code == TypeCode::F64) return IR_DOUBLE;
  else return IR_ADDR;
}
```

### Complete Mapping Example

Here's how `(i32.const 10) (i32.const 20) (i32.add)` gets mapped:

```cpp
// 1. i32.const 10
case OpCode::I32__const:
  ConstVal = ir_CONST_I32(10);   // Creates CONST node
  push(ConstVal);                 // Stack: [ref_10]

// 2. i32.const 20  
case OpCode::I32__const:
  ConstVal = ir_CONST_I32(20);   // Creates CONST node
  push(ConstVal);                 // Stack: [ref_10, ref_20]

// 3. i32.add
case OpCode::I32__add:
  Right = pop();                  // ref_20, Stack: [ref_10]
  Left = pop();                   // ref_10, Stack: []
  Result = ir_ADD_I32(Left, Right);  // Creates ADD node with edges
  push(Result);                   // Stack: [ref_add]
```

The resulting IR graph:
```
  ┌─────────┐     ┌─────────┐
  │ CONST   │     │ CONST   │
  │   10    │     │   20    │
  └────┬────┘     └────┬────┘
       │               │
       ▼               ▼
       └───────┬───────┘
               │
        ┌──────▼──────┐
        │    ADD      │
        │   (i32)     │
        └─────────────┘
```

### Adding a New Instruction Mapping

To add a new WebAssembly instruction:

1. **Add to dispatch switch** in `visitInstruction()`:
```cpp
case OpCode::I32__rotl:
  return visitBinary(Op);
```

2. **Add handling** in the appropriate visitor:
```cpp
case OpCode::I32__rotl:
  Result = ir_ROL_I32(Left, Right);  // Use corresponding IR macro
  break;
```

3. **Add test** in `test/ir/ir_instruction_test.cpp`:
```cpp
testBinaryOp(OpCode::I32__rotl);
```

### Codegen and integration choices

1. **Stack-allocated IR context** — Following `ir/examples/`, contexts are stack-allocated for performance.

2. **SSA-form locals** — WebAssembly locals map to SSA values, not memory (no load/store needed for locals).

3. **Visitor pattern** — Opcodes dispatch as in the steps above (`visitInstruction` → category handlers such as `visitBinary`, `visitControl`, `visitCall`, `visitRefType`, `visitTable`).

4. **CMake** — `WASMEDGE_BUILD_IR_JIT` defaults **ON** in the root `CMakeLists.txt`; set **`OFF`** to drop IR JIT. Details: [Build & Test & Debug](#build--test--debug) §7.

5. **`JitExecEnv` + args buffer** — Generated code receives **`JitExecEnv* env`** and **`uint64_t* args`** (Wasm parameters packed by `IRJitEngine::invoke`). The builder loads **`env->MemoryBase`**, **`env->GlobalBase`**, **`env->FuncTable`**, and helper function pointers to implement memory, globals, calls, `memory.size` / `memory.grow`, table/bulk ops, and traps (see §3.2).

6. **Reference types on the stack** — **`funcref` / `externref`** values use **two** logical stack slots (type + pointer), with **`LabelInfo`** merge metadata for ref-typed block results (`RefBranchResults` / `mergeRefResults`).

7. **Shared call argument buffer** — A pre-scan sets **`MaxCallArgs`**; the builder allocates a **shared** `uint64_t` buffer for outgoing calls to avoid per-call **`ir_ALLOCA`** in hot paths.

8. **Compile validation and failure** — **`ir_check`** runs before **`ir_jit_compile`**. If lowering or compile fails, the function **stays** a normal **`WasmFunction`** and runs on the **interpreter** (graceful degradation).

9. **`dstogov/ir` guard/snapshot features** — The IR framework supports **guards** and **snapshots** for speculative optimization (thesis [§5](#5-core-design-principles-of-dstogov-ir)). **WasmEdge does not currently wire speculative guards** for Wasm; this integration is **baseline lowering + `ir_jit_compile`**, not speculative tiered IR.

---

## 6. Recap: key design decisions

| Topic | Choice |
|--------|--------|
| IR storage | `ir_ctx` stack-allocated in `WasmToIRBuilder`; optional **retain** `IRGraph` in `IRJitFunction` for future tier-up (often **`nullptr`** today). |
| Code memory | `mmap` RW → encode → **RX** (`mprotect`). |
| Wasm params | Packed into **`uint64_t[]`**; floats use raw bit patterns in the low bits of slots. |
| Module context | Set **after** `initialize()` so **`reset()`** does not drop module-scoped tables. |
| SIMD / atomics / exceptions | Not implemented in IR JIT (see [Implementation Status](#implementation-status)). |

---

# Build & Test & Debug

This section covers configuring and building WasmEdge with IR JIT, running tests, quick verification, and **debugging** (GDB, IR dumps, Sightglass env knobs). The summary at the top of this document under **Build defaults and debugging** lists the main env vars; details below expand on that.

## Prerequisites

- CMake 3.11+
- C++20 compiler (GCC 11+, Clang 13+)
- Standard WasmEdge dependencies
- `make`, `gcc`/`clang` for building `dstogov/ir`

## 1. Clone with IR submodule

To ensure the `dstogov/ir` framework is available when you clone:

```bash
git clone --recursive https://github.com/WasmEdge/WasmEdge.git
cd WasmEdge
```

If you already cloned the repo without `--recursive`, initialize submodules:

```bash
git submodule update --init --recursive
```

This will populate `thirdparty/ir` with the `dstogov/ir` sources.

The IR JIT integration treats `dstogov/ir` as an external project built with its own Makefile. 
Thanks to WasmEdge's CMake configuration, **you do not need to build `libir.a` manually**. 
When **`WASMEDGE_BUILD_IR_JIT`** is **ON** (the CMake default), the build system configures and builds the `dstogov/ir` submodule for your CPU architecture (x86_64 vs. AArch64) and links it into `libwasmedge`.

## 2. Configure and build WasmEdge with IR JIT

From the WasmEdge repo root:

```bash
mkdir -p build
cd build

# Explicit Release + tests; IR JIT + LLVM match CMake defaults (both ON unless you override)
cmake -DCMAKE_BUILD_TYPE=Release \
      -DWASMEDGE_BUILD_IR_JIT=ON \
      -DWASMEDGE_BUILD_TESTS=ON \
      ..

# Build everything
make -j32
```

Key flags:

- **`WASMEDGE_BUILD_IR_JIT`**: defaults to **`ON`** in CMake; set **`OFF`** to exclude the IR baseline JIT and `thirdparty/ir`. When **ON**, CMake builds the `dstogov/ir` submodule and links it into `libwasmedge`.
- **`WASMEDGE_BUILD_TESTS=ON`**: ensures the unit, integration, and e2e test suites are built.
- **`WASMEDGE_USE_LLVM`**: defaults to **`ON`**; set **`OFF`** for a build without the LLVM-based compilation runtime. **Sightglass AOT** and other LLVM-dependent tests need it **ON** (benchmark test links `wasmedgeLLVM` when enabled).

For **GDB** (backtraces, stepping), prefer **`CMAKE_BUILD_TYPE=Debug`** when reconfiguring (`cmake -DCMAKE_BUILD_TYPE=Debug ..` from `build/`). Debug WasmEdge also drives a **debug** build of `thirdparty/ir` via CMake (assertions, better symbols). See also [§9. Debug](#9-debug).

## 3. Building Specific Components

For faster iteration during development, you can build specific parts of the project in isolation:

### Building the IR Submodule Only

CMake builds `thirdparty/ir/libir.a` automatically when you run `make` inside the WasmEdge build directory. To trigger it in isolation:

```bash
cd build
make wasmedgeIRBuild
```

**Manual build** — if you need to build `libir.a` by hand directly in the submodule:

```bash
cd thirdparty/ir
make clean

# Debug build (assertions enabled, better GDB symbols — used by WasmEdge CMake by default)
make BUILD=debug libir.a

# Release build (no assertions, optimised)
make BUILD=release libir.a

# On Linux, add -fPIC so libir.a can link into libwasmedge.so:
CFLAGS=-fPIC BUILD_CFLAGS=-fPIC make BUILD=debug libir.a

# On AArch64 (Apple Silicon, ARM servers):
make BUILD=debug TARGET=aarch64 libir.a
```

> **Note:** WasmEdge’s CMake chooses the IR makefile **`BUILD=debug`** when **`CMAKE_BUILD_TYPE=Debug`** and **`BUILD=release`** when the WasmEdge build is not Debug (`thirdparty/CMakeLists.txt`, `_ir_make_args`). There is no separate “always debug libir” mode for Release WasmEdge builds.

### Building Specific Core Libraries
To build just the individual static libraries (useful when modifying specific subsystems):
```bash
cd build
make wasmedgeVM -j32
make wasmedgeExecutor -j32
make wasmedgeLoader -j32
```

### Building Specific Test Suites
To compile just the test executables (much faster than building everything):
```bash
cd build
make wasmedgeIRTests wasmedgeIRInstructionTests -j32
make wasmedgeIRExecutionTests wasmedgeIRIntegrationTests wasmedgeIRE2ETests wasmedgeIRBenchmarkTests -j32
```

### Verfication
With **`WASMEDGE_BUILD_IR_JIT=ON`** (the default), check that IR symbols are present:

```bash
nm thirdparty/ir/libir.a | grep "T ir_jit_compile"
# Should show: T ir_jit_compile (or similar)

nm lib/vm/libwasmedgeVM.a | grep "WasmToIRBuilder"
# Should show WasmEdge::VM::WasmToIRBuilder symbols
```

## 4. Running the Tests

**Full IR test suite (Execution, Benchmarks, E2E, Unit, Integration):**
```bash
cd build
ctest -R IR -V --output-on-failure
```

**Individual test executables:**
```bash
cd build
./test/ir/wasmedgeIRTests
./test/ir/wasmedgeIRInstructionTests
./test/ir/wasmedgeIRExecutionTests
./test/ir/wasmedgeIRE2ETests
./test/ir/wasmedgeIRBenchmarkTests
./test/ir/wasmedgeIRIntegrationTests
```

**Run a specific test case:**
```bash
cd build
./test/ir/wasmedgeIRE2ETests --gtest_filter=IRE2ETest.LoadAndRunAdd
./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

**Sightglass benchmark suite (Interpreter vs JIT vs AOT):**

**Environment variables for Sightglass:**

| Variable | Effect |
|----------|--------|
| `WASMEDGE_SIGHTGLASS_KERNEL` | Run only this kernel (e.g. `noop`, `quicksort`; with or without `.wasm`). |
| `WASMEDGE_SIGHTGLASS_MODE` | Run only this mode: `Interpreter`, `IR_JIT`, `JIT`, or `AOT`. Use **`IR_JIT`** to exercise only the IR JIT column. |
| `WASMEDGE_SIGHTGLASS_QUICK` | `1` (default in some paths) runs a subset; set **`0`** to run every `*.wasm` under `test/ir/testdata/sightglass/`. |
| `WASMEDGE_IR_JIT_OPT_LEVEL` | `0`, `1`, or `2` — passed through to `ir_jit_compile` (default **2**). Lower when chasing codegen issues. |
| `WASMEDGE_IR_JIT_BOUND_CHECK` | Set to **`1`** to emit extra in-IR memory bounds checks (see `WasmToIRBuilder::buildBoundsCheck`). |
| `WASMEDGE_SIGHTGLASS_SKIP_INTERP=1` | Skip Interpreter (often slow) in `SightglassSuite`. |
| `WASMEDGE_SIGHTGLASS_SKIP_AOT=1` | In test: skip AOT (e.g. when not built with LLVM). |
| `SIGHTGLASS_TIMEOUT` | Per-run timeout in seconds (script only; default 15). |

**IR JIT–only Sightglass example** (from `build/`):

```bash
cd build
WASMEDGE_SIGHTGLASS_MODE=IR_JIT
WASMEDGE_IR_JIT_OPT_LEVEL=2
WASMEDGE_SIGHTGLASS_QUICK=0   # all kernels; omit or use 1 for quick sweep
WASMEDGE_SIGHTGLASS_KERNEL=quicksort timeout 30 ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

**Loop all Sightglass kernels (IR JIT only):**

```bash
cd build
for wasm in ../test/ir/testdata/sightglass/*.wasm; do
  kernel="$(basename "$wasm" .wasm)"
  echo "Testing $kernel:"
  WASMEDGE_SIGHTGLASS_KERNEL="$kernel" WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
    WASMEDGE_SIGHTGLASS_QUICK=1 WASMEDGE_IR_JIT_OPT_LEVEL=2 WASMEDGE_IR_JIT_BOUND_CHECK=0 \
    stdbuf -oL timeout 30 ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*' 2>&1
done | tee /tmp/wasm-test.log
```

## 5. Debug

#### Debug builds and fast targets

- Reconfigure with **`CMAKE_BUILD_TYPE=Debug`** for clearer stack traces and symbols (then `cmake --build .` or target-specific builds).
- Build only what you need, e.g. `cmake --build . --target wasmedgeIRBenchmarkTests -j$(nproc)`.
- To rebuild **`libir.a`** by hand with PIC (e.g. Linux shared linking): `CFLAGS=-fPIC BUILD_CFLAGS=-fPIC make BUILD=debug libir.a` in `thirdparty/ir` (see [§4. Building Specific Components](#4-building-specific-components)).

### GDB and JIT-generated code

Run under GDB from `build/` with env vars set (example: single Sightglass kernel, IR JIT, opt level 2):

```bash
cd build
WASMEDGE_SIGHTGLASS_MODE=IR_JIT WASMEDGE_SIGHTGLASS_KERNEL=quicksort WASMEDGE_IR_JIT_OPT_LEVEL=2 \
  gdb --args ./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

JIT regions are registered with **`ir_gdb_register`** as symbols named **`wasm_jit_NNN`** (`000`, `001`, …). After a fault in generated code, shallow backtraces are common; use:

| Goal | GDB |
|------|-----|
| Faulting instruction | `x/i $pc` |
| Nearby machine code | `disas $pc-32,$pc+32` |
| Register / pointer state | `info registers` |
| Correlate `wasm_jit_NNN` to a Wasm function | `NNN` is a running index of JIT-compiled functions in that process. Set breakpoints on **`WasmEdge::Executor::enterFunction`** or **`WasmEdge::VM::IRJitEngine::invoke`**, run, then see which kernel / call path runs before continuing into the crash. |

### IR text dumps (`WASMEDGE_IR_JIT_DUMP`)

If **`WASMEDGE_IR_JIT_DUMP`** is set to any non-empty value, `IRJitEngine::compile` (`lib/vm/ir_jit_engine.cpp`) writes **per compiled Wasm function** (in compile order):

1. **`/tmp/wasmedge_ir_NNN_before.ir`** — IR text before `ir_jit_compile` (`ir_save` without CFG extras).
2. **`/tmp/wasmedge_ir_NNN_after.ir`** — after successful compile (`ir_save` with **`IR_SAVE_CFG`**; see `ir.h`).

Use **`WASMEDGE_IR_JIT_OPT_LEVEL=0`** (or `1`) when isolating codegen issues; dumps reflect the graph at that optimization level. The standalone **`thirdparty/ir`** `ir` tool supports extra **`--save-ir-after-*`** flags, but those are **not** wired into WasmEdge’s JIT path—only the env-driven pair above unless you add more hooks.

Example:

```bash
WASMEDGE_IR_JIT_DUMP=1 WASMEDGE_IR_JIT_OPT_LEVEL=2 ./test/ir/wasmedgeIRE2ETests --gtest_filter='*'
# Inspect /tmp/wasmedge_ir_*.ir
```

---

# Testing Strategy

Tests live under `test/ir/` (Google Test). They check **lowering → `ir_jit_compile` → native execution** in layers, with **196** `TEST`/`TEST_F` cases total across six binaries. **Opcode-by-opcode breakdowns** are in [Implementation Status](#implementation-status); this section avoids duplicating those tables.

## Goals

| Goal | How it is tested |
|------|------------------|
| Lowering builds valid IR | Levels 1–2: `buildFromInstructions` succeeds; `ir_instruction_test` sweeps categories |
| Backend accepts the graph | Level 3: `IRJitEngine::compile` / `ir_check` |
| Results match Wasm semantics | Level 4: `ir_execution_test` runs via `IRJitEngine::invoke` (**`JitExecEnv`** ABI, same shape as the executor) |
| Real modules and backends | Integration, E2E, benchmarks (Sightglass compares Interpreter / IR JIT / LLVM AOT when built) |

## Four levels (compact)

| Level | Question | Primary sources |
|-------|----------|-----------------|
| **1** | Does the opcode lower? | `ir_basic_test.cpp` |
| **2** | Do whole categories lower? | `ir_instruction_test.cpp` (helpers like `testBinaryOp`, table/bulk/ref smoke) |
| **3** | Does `ir_jit_compile` succeed? | Mixed in basic + instruction tests; `Compile_All_I32_Ops` style checks |
| **4** | Does native code compute correctly? | `ir_execution_test.cpp` (81 tests: mostly **i32/i64**, plus **8** control, **19** memory, **4** calls, **5** globals, **2** ref) |

**Note:** Float ops are **exercised at IR level** in `ir_instruction_test`; there is **no** large **f32/f64 execution** suite yet.

## Suite inventory

| Executable | Source | Cases | Role |
|------------|--------|------:|------|
| `wasmedgeIRTests` | `ir_basic_test.cpp` | 33 | Small IR + compile smoke |
| `wasmedgeIRInstructionTests` | `ir_instruction_test.cpp` | 58 | Category lowering + JIT compile |
| `wasmedgeIRExecutionTests` | `ir_execution_test.cpp` | 81 | Correctness via `invoke` |
| `wasmedgeIRIntegrationTests` | `ir_integration_test.cpp` | 6 | VM `invoke` path (factorial, memory, globals, etc.) |
| `wasmedgeIRE2ETests` | `ir_e2e_test.cpp` | 5 | `testdata/*.wasm` through loader + instantiation |
| `wasmedgeIRBenchmarkTests` | `ir_benchmark_test.cpp` | 13 | Algorithms + **`SightglassSuite`** |
| **Total** | | **196** | |

**Layout:** `test/ir/` also has `run_sightglass_all.sh`, `testdata/` (`factorial`, `fibonacci`, `sightglass/`), and `CMakeLists.txt`.

## End-to-end loader tests

`wasmedgeIRE2ETests` loads real `.wasm` files (e.g. under `test/ir/testdata/`) through the normal loader and instantiation path with IR JIT enabled:

| Test | Description | Status |
|------|-------------|--------|
| LoadAndRunFactorial | Iterative factorial (loop + locals) | ✅ JIT |
| LoadAndRunAdd | Simple addition | ✅ JIT |
| LoadAndRunWithFunctionCall | Function calling another function | ✅ JIT |
| LoadAndRunMemoryOps | Memory store/load | ✅ JIT |
| LoadAndRunGlobalOps | Global get/set/increment | ✅ JIT |

Each case passes under IR JIT when enabled; modules are compiled from `.wat` where applicable.

### How execution tests are structured

Fixtures build minimal Wasm, **lower**, **compile**, then call **`IRJitEngine::invoke`** with `FuncTable` / `MemoryBase` as needed (see `Call_DirectToNative`, `Memory_*`, `invokeI32` helpers in `ir_execution_test.cpp`). Miscompilation of **sign extension**, **branch merge**, or **calls** shows up as wrong numeric results.

## Sightglass and benchmarks

**Benchmark** binary runs **`SightglassSuite`**: real `.wasm` kernels under **Interpreter**, **IR JIT**, **LLVM JIT**, and **LLVM AOT**. Outputs (stdout/stderr/exit) are cross-checked across modes.

## Running tests

```bash
cd build
make -j32 wasmedgeIRTests wasmedgeIRInstructionTests wasmedgeIRExecutionTests \
         wasmedgeIRIntegrationTests wasmedgeIRE2ETests wasmedgeIRBenchmarkTests
ctest -R IR -V --output-on-failure
```

Individual binaries: `./test/ir/wasmedgeIRTests`, `wasmedgeIRInstructionTests`, `wasmedgeIRExecutionTests`, `wasmedgeIRIntegrationTests`, `wasmedgeIRE2ETests`, `wasmedgeIRBenchmarkTests`.

## Gaps

| Area | Notes |
|------|--------|
| **Float execution** | No dedicated f32/f64 exec battery (IR mapping exists) |
| **Conversions** | Many covered at IR level; execution coverage thinner |
| **Edge cases** | NaN/overflow/underflow could use more tests |

---

# References

## Key Files to Reference

- **IR Examples**: `thirdparty/ir/examples/` (or `ir/examples/` in the dstogov/ir repo)
  - `0001-basic.c` - Basic function compilation pattern
  - `0003-pointer.c` - Memory operations pattern
  - `0004-func.c` - Function call pattern

- **IR Headers**: `thirdparty/ir/`
  - `ir.h` - Core IR types and functions
  - `ir_builder.h` - Builder macros (ir_PARAM, ir_ADD, etc.)

- **WasmEdge executor**: `lib/executor/helper.cpp` — `enterFunction` / `invoke` IR JIT branch (`isIRJitFunction` → `IRJitEngine::invoke`), plus `jit_host_call`, `jit_call_indirect`, and related trampolines.

## Related Documentation

See `notes/` directory for additional documentation:
- Build instructions
- Architecture details
- Performance notes
- Development logs

## External Resources

- **dstogov/ir**: https://github.com/dstogov/ir
- **WebAssembly Spec**: https://webassembly.github.io/spec/
- **WasmEdge**: https://wasmedge.org/

---

# Implementation Status

This section provides a comprehensive breakdown of all WebAssembly instruction implementations.

## Objective
  - ✅ Integrate `dstogov/ir` into WasmEdge (`thirdparty/CMakeLists.txt` → `wasmedgeIR` imported target, `wasmedgeIRBuild`)
  - ✅ Wasm→IR lowering (`WasmToIRBuilder`)
  - ✅ JIT engine (`IRJitEngine`: mmap buffers, `ir_jit_compile`, GDB JIT registration, `invoke`)
  - ✅ Execution tests (**81** in `ir_execution_test.cpp`) plus integration / E2E / benchmarks
  - ✅ Executor path: `enterFunction` / `invoke` builds **`JitExecEnv`**, per-module **func table + global pointer array** cache, calls native code
  - ⏳ Tier-up to LLVM / retain `ir_ctx` for optimization (not implemented; `upgradeToIRJit(..., nullptr)` today)

## ✅ Fully Implemented

| Category | Count | Instructions |
|----------|-------|--------------|
| **Constants** | 4 | `i32.const`, `i64.const`, `f32.const`, `f64.const` |
| **Locals** | 3 | `local.get`, `local.set`, `local.tee` |
| **I32 Binary** | 15 | `add`, `sub`, `mul`, `div_s`, `div_u`, `rem_s`, `rem_u`, `and`, `or`, `xor`, `shl`, `shr_s`, `shr_u`, `rotl`, `rotr` |
| **I64 Binary** | 15 | `add`, `sub`, `mul`, `div_s`, `div_u`, `rem_s`, `rem_u`, `and`, `or`, `xor`, `shl`, `shr_s`, `shr_u`, `rotl`, `rotr` |
| **F32 Binary** | 6 | `add`, `sub`, `mul`, `div`, `min`, `max` |
| **F64 Binary** | 6 | `add`, `sub`, `mul`, `div`, `min`, `max` |
| **I32 Compare** | 10 | `eq`, `ne`, `lt_s`, `lt_u`, `le_s`, `le_u`, `gt_s`, `gt_u`, `ge_s`, `ge_u` |
| **I64 Compare** | 10 | `eq`, `ne`, `lt_s`, `lt_u`, `le_s`, `le_u`, `gt_s`, `gt_u`, `ge_s`, `ge_u` |
| **F32 Compare** | 6 | `eq`, `ne`, `lt`, `le`, `gt`, `ge` |
| **F64 Compare** | 6 | `eq`, `ne`, `lt`, `le`, `gt`, `ge` |
| **I32 Unary** | 4 | `eqz`, `clz`, `ctz`, `popcnt` |
| **I64 Unary** | 4 | `eqz`, `clz`, `ctz`, `popcnt` |
| **F32 Unary** | 2 | `abs`, `neg` |
| **F64 Unary** | 2 | `abs`, `neg` |
| **Parametric** | 2 | `drop`, `select` |
| **Control Flow** | 12 | `nop`, `unreachable`, `block`, `loop`, `if`, `else`, `end`, `br`, `br_if`, `br_table`, `return`, `call`, `call_indirect` |
| **Globals** | 2 | `global.get`, `global.set` |
| **Type Conversion** | 37 | `i32.wrap_i64`, `i64.extend_i32_s/u`, `i32.trunc_f32_s/u`, `i32.trunc_f64_s/u`, `i64.trunc_f32_s/u`, `i64.trunc_f64_s/u`, `i32.trunc_sat_f32_s/u`, `i32.trunc_sat_f64_s/u`, `i64.trunc_sat_f32_s/u`, `i64.trunc_sat_f64_s/u`, `f32.convert_i32_s/u`, `f32.convert_i64_s/u`, `f64.convert_i32_s/u`, `f64.convert_i64_s/u`, `f32.demote_f64`, `f64.promote_f32`, `i32.reinterpret_f32`, `i64.reinterpret_f64`, `f32.reinterpret_i32`, `f64.reinterpret_i64`, `i32.extend8_s`, `i32.extend16_s`, `i64.extend8_s`, `i64.extend16_s`, `i64.extend32_s` |
| **Memory Load** | 14 | `i32.load`, `i64.load`, `f32.load`, `f64.load`, `i32.load8_s/u`, `i32.load16_s/u`, `i64.load8_s/u`, `i64.load16_s/u`, `i64.load32_s/u` |
| **Memory Store** | 9 | `i32.store`, `i64.store`, `f32.store`, `f64.store`, `i32.store8/16`, `i64.store8/16/32` |
| **Table Ops** | 8 | `table.get`, `table.set`, `table.size`, `table.grow`, `table.fill`, `table.copy`, `table.init`, `elem.drop` |
| **Reference Types** | 3 | `ref.null`, `ref.is_null`, `ref.func` |
| **Bulk Memory** | 4 | `memory.copy`, `memory.fill`, `memory.init`, `data.drop` |
| **Memory Size/Grow** | 2 | `memory.size`, `memory.grow` |
| **TOTAL** | **183** | |

## ⚠️ Placeholder Implementations (Dispatch but don't work correctly)

| Category | Count | Instructions | Issue |
|----------|-------|--------------|-------|
| **F32 Math** | 5 | `sqrt`, `ceil`, `floor`, `trunc`, `nearest` | Return operand unchanged (need intrinsic calls) |
| **F64 Math** | 5 | `sqrt`, `ceil`, `floor`, `trunc`, `nearest` | Return operand unchanged (need intrinsic calls) |
| **TOTAL** | **10** | | |

## ❌ Not Implemented (Will return error)

| Category | Count | Instructions |
|----------|-------|--------------|
| **SIMD (v128)** | 200+ | All v128 operations |
| **Atomics** | 50+ | All atomic operations |
| **Exceptions** | 6+ | `try`, `catch`, `throw`, etc. |
| **TOTAL** | **~276+** | |

## Summary

| Status | Count | Notes |
|--------|-------|--------|
| ✅ Implemented (listed categories) | 183 | Scalar / memory / table / bulk / refs as in table above |
| ⚠️ Placeholder | 10 | f32/f64 `sqrt`, `ceil`, `floor`, `trunc`, `nearest` (identity lowering) |
| ❌ Not implemented | — | **SIMD (v128)**, **atomics**, **exceptions** — not in IR JIT |

*Percentages in older revisions were misleading; treat SIMD/atomics/exceptions as unsupported until explicitly added.*

## Priority for Completion

**High Priority** (required for basic programs):
1. ✅ ~~Type conversions (37)~~ - **COMPLETED**
2. ✅ ~~Memory load/store~~ — **DONE** (`JitExecEnv::MemoryBase` / outlined helpers)
3. ✅ ~~Global operations~~ — **DONE** (`GlobalBase` as `ValVariant**`)
4. ✅ ~~Function calls~~ — **DONE** (`FuncTable` + trampolines for imports / `call_indirect`)

**Medium Priority** (common features):
5. ⚠️ Float math (10) - `sqrt`, `ceil`, `floor`, etc.
6. ✅ ~~`memory.size/grow` (2)~~ - **COMPLETED**

**Low Priority** (advanced features):
8. ✅ ~~Table operations (8)~~ - **COMPLETED**
9. ✅ ~~Reference types (3)~~ - **COMPLETED**
10. ✅ ~~Bulk memory (4)~~ - **COMPLETED** (`memory.copy`, `memory.fill`, `memory.init`, `data.drop`; copy/fill use bounds-checking helpers)

## Recommended Next Steps

1. **Float Math Intrinsics** (10 instructions) - Quick win  
   Fix placeholders by adding `ir_CALL` to C library functions (`sqrtf`, `sqrt`, `ceilf`, `ceil`, etc.)

2. ~~**Memory Size/Grow & Bulk Memory**~~ - **DONE** (memory.size, memory.grow, memory.copy, memory.fill, memory.init, data.drop via JIT helpers with bounds checking)

---

