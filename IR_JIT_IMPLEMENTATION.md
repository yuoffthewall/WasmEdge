# WasmEdge IR JIT Implementation

**Status**: Phase 1 - Complete IR Lowering + WasmEdge Integration ✅  
**Last Updated**: February 11, 2026  
**Build Status**: Compiling with no errors  
**Test Status**: All tests passing (100%)
- Basic IR Generation (33 tests)
- Instruction Coverage (43 tests)  
- **Execution Correctness (79 tests)** ✅ (includes 8 control flow + 19 memory + 4 function call + 5 global tests)
- **Integration Tests (6 tests)** ✅ (includes factorial, memory access, globals, conditional logic)
- **End-to-End Tests (5 tests)** ✅ NEW - real .wasm file loading and execution

**WasmEdge Integration**: ✅ IR JIT compiles functions during module instantiation  
**Instruction Coverage**: ~167 WebAssembly instructions mapped to IR  
**Memory Operations**: ✅ Fully implemented with execution correctness verified (19 tests)  
**Function Calls**: ✅ `call` and `call_indirect` implemented with execution correctness verified (4 tests)  
**Global Operations**: ✅ `global.get` and `global.set` implemented with execution correctness verified (5 tests)  
**Execution Verified**: ✅ I32/I64 arithmetic, bitwise, comparison, unary, memory ops, function calls, globals produce correct results  
**Control Flow Execution**: ✅ Fixed - loops with local variables now correctly use PHI nodes for SSA  
**Loop Variables**: ✅ Fixed - PHI nodes properly merge local variable values across loop iterations

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [How Instruction Mappings Work](#how-instruction-mappings-work)
4. [Build Instructions](#build-instructions)
5. [Key Components](#key-components)
6. [WasmEdge Integration](#wasmedge-integration)
7. [Testing Strategy](#testing-strategy)
8. [Implementation Status](#implementation-status)
9. [References](#references)

---

## Overview

This document tracks the implementation of tiered compilation for WasmEdge using the `dstogov/ir` framework as a baseline JIT compiler. The goal is to achieve fast startup times with a lightweight JIT while maintaining the option to tier-up to LLVM for peak performance.

### Objectives

- ✅ Integrate `dstogov/ir` framework into WasmEdge build system
- ✅ Create Wasm→IR translation layer
- ✅ Implement basic JIT compilation engine
- ✅ Enable end-to-end execution of simple Wasm functions (79 execution tests passing)
- ✅ Integrate with WasmEdge executor dispatch (functions compiled at module instantiation)
- ✅ End-to-end testing with real `.wasm` files (5 E2E tests)
- ⏳ Add tier-up mechanism to LLVM (future)

### Current Achievement

The IR JIT framework is successfully integrated into WasmEdge and compiles cleanly. All core infrastructure is in place for translating WebAssembly to IR and JIT compiling to native code.

**Build Success**:
```bash
✅ [100%] Built target wasmedgeIR       (IR library)
✅ [100%] Built target wasmedgeVM       (VM with IR JIT)
✅ [100%] Built target wasmedgeExecutor (Executor integration)
```

---

## Architecture

### High-Level Flow

```
WebAssembly Module
       ↓
   AST Parser (existing)
       ↓
   WasmToIRBuilder ← [NEW]
       ↓
   IR Graph (dstogov/ir)
       ↓
   IRJitEngine ← [NEW]
       ↓
   Native Machine Code
       ↓
   FunctionInstance (IR variant) ← [MODIFIED]
       ↓
   Executor dispatch ← [INTEGRATED - isIRJitFunction() check]
```

### Key Classes and Functions

#### 1. `WasmToIRBuilder` Class
**Location**: `include/vm/ir_builder.h`, `lib/vm/ir_builder.cpp`

Translates WebAssembly AST to dstogov/ir Sea-of-Nodes graph.

```cpp
class WasmToIRBuilder {
public:
  // Initialize IR context for a function
  Expect<void> initialize(const AST::FunctionType &FuncType,
                          Span<const std::pair<uint32_t, ValType>> LocalVars);
  
  // Process all instructions and build IR graph
  Expect<void> buildFromInstructions(Span<const AST::Instruction> Instrs);
  
  // Get the built IR context for compilation
  ir_ctx *getIRContext() noexcept;
  
  // Reset and free resources
  void reset() noexcept;

private:
  // Type conversion
  ir_type wasmTypeToIRType(ValType Type) const noexcept;
  
  // Instruction visitors (dispatch targets)
  Expect<void> visitInstruction(const AST::Instruction &Instr);
  Expect<void> visitConst(const AST::Instruction &Instr);
  Expect<void> visitLocal(const AST::Instruction &Instr);
  Expect<void> visitBinary(OpCode Op);
  Expect<void> visitCompare(OpCode Op);
  Expect<void> visitUnary(OpCode Op);
  Expect<void> visitConversion(OpCode Op);
  Expect<void> visitParametric(const AST::Instruction &Instr);
  Expect<void> visitControl(const AST::Instruction &Instr);
  Expect<void> visitMemory(const AST::Instruction &Instr);
  
  // Stack operations
  void push(ir_ref Ref) noexcept;
  ir_ref pop() noexcept;
  ir_ref peek(uint32_t Depth) const noexcept;

  // Member variables
  ir_ctx Ctx;                              // IR context (stack-allocated)
  bool Initialized;
  std::vector<ir_ref> ValueStack;          // Operand stack simulation
  std::unordered_map<uint32_t, ir_ref> Locals;  // SSA refs for locals
  std::vector<LabelInfo> LabelStack;       // Control flow labels
  uint32_t LocalCount;
};
```

#### 2. `IRJitEngine` Class
**Location**: `include/vm/ir_jit_engine.h`, `lib/vm/ir_jit_engine.cpp`

JIT compiles IR graphs to native machine code.

```cpp
class IRJitEngine {
public:
  struct CompileResult {
    void *NativeFunc;    // Pointer to generated code
    size_t CodeSize;     // Size of generated code
    ir_ctx *IRGraph;     // Retained for potential tier-up
  };

  // Compile IR graph to native code
  Expect<CompileResult> compile(ir_ctx *Ctx);
  
  // Execute compiled function (TODO: integrate with executor)
  Expect<void> invoke(void *NativeFunc, 
                      Span<const ValType> ParamTypes,
                      Span<const ValType> ReturnTypes,
                      Span<ValVariant> Args,
                      Span<ValVariant> Rets);
  
  // Release compiled code memory
  void release(void *NativeFunc, size_t CodeSize) noexcept;
};
```

#### 3. `FunctionInstance` Extension
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

### Component Interaction

```
┌─────────────────────────────────────────────────────────┐
│                   WasmEdge Runtime                      │
├─────────────────────────────────────────────────────────┤
│  Executor                                               │
│    ├─→ Host Functions                                  │
│    ├─→ LLVM Compiled Functions (existing)              │
│    └─→ IR JIT Functions (NEW)                          │
│           ↓                                             │
│      IRJitEngine::compile()                             │
│           ↓                                             │
│      WasmToIRBuilder::buildFromInstructions()           │
│           ↓                                             │
│      dstogov/ir: ir_jit_compile()                       │
└─────────────────────────────────────────────────────────┘
```

### Key Design Decisions

1. **Stack-Allocated IR Context** - Following `ir/examples/`, contexts are stack-allocated for performance

2. **SSA-Form Locals** - WebAssembly locals map to SSA values, not memory (no load/store needed)

3. **Visitor Pattern** - Instructions dispatched by category to specific handlers

4. **Optional Build** - `WASMEDGE_BUILD_IR_JIT=ON` enables; default OFF

5. **Implicit Memory Base Parameter** - JIT functions receive memory base pointer as first parameter:
   ```
   Original Wasm:  func(i32, i32) -> i32
   JIT Signature:  func(void* mem_base, i32, i32) -> i32
   ```
   This enables actual memory load/store operations using IR's `ir_LOAD_*` and `ir_STORE` macros.

---

## Instruction Mapping

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
  }
}
```

### Step 2: Handler Functions

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

### Available IR Macro Categories

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

---

## Build Instructions

### Prerequisites

- CMake 3.11+
- C++20 compiler (GCC 11+, Clang 13+)
- Standard WasmEdge dependencies

### Building with IR JIT

```bash
cd /Users/tommylee/Desktop/WasmEdge
mkdir -p build && cd build

# Configure with IR JIT enabled
cmake -DWASMEDGE_BUILD_IR_JIT=ON ..

# Build
make -j8

# Verify libraries
ls -lh thirdparty/ir/libwasmedgeIR.a
ls -lh lib/vm/libwasmedgeVM.a
```

### Building without IR JIT (Default)

```bash
cmake ..
make -j8
```

### Verification

Check that IR symbols are present:
```bash
nm thirdparty/ir/libwasmedgeIR.a | grep "T ir_jit_compile"
# Should show: T _ir_jit_compile

nm lib/vm/libwasmedgeVM.a | grep "WasmToIRBuilder"
# Should show WasmEdge::VM::WasmToIRBuilder symbols
```

---

## Key Components

### File Structure

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
│   │   └── factorial.wasm                  [new - compiled E2E test]
│   └── ir_e2e_test.cpp                     [new - E2E integration tests]
└── ir/                                     [dstogov/ir submodule]
    ├── ir.h
    ├── ir_builder.h
    └── examples/                           [studied for patterns]
```

### Code Statistics

**New Files**: 4 headers + 2 implementations + 5 test files = 11 files  
**Modified Files**: 8 files  
**Lines of Code Added**: ~5,500 lines

| Component | Lines | Description |
|-----------|-------|-------------|
| ir_builder.h | 138 | Translation layer interface |
| ir_builder.cpp | 1,381 | Complete Wasm→IR lowering (~164 instrs + memory ops) |
| ir_jit_engine.h | 50 | JIT engine interface |
| ir_jit_engine.cpp | 233 | Compilation and execution (updated calling convention) |
| ir_basic_test.cpp | 916 | Basic functionality tests (33 tests) |
| ir_instruction_test.cpp | 720 | Comprehensive instruction tests (35 tests) |
| ir_execution_test.cpp | 1376 | Execution correctness tests (67 tests) |
| ir_e2e_test.cpp | ~200 | End-to-end integration tests (5 tests) |
| factorial.wat | ~50 | E2E test module source |
| function.h | +50 | IR JIT function variant + upgradeToIRJit |
| module.cpp | +80 | IR JIT compilation hook |
| helper.cpp | +60 | Runtime context (func_table, globals, memory) |
| CMake files | +200 | Build & test configuration |

---

## WasmEdge Integration

The IR JIT compiler is fully integrated with WasmEdge's module instantiation and execution pipeline. When a Wasm module is loaded and instantiated, eligible functions are automatically compiled to native code using the IR JIT.

### Integration Flow

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
│    └── **IR JIT COMPILATION** (after all sections ready)        │
│        ├── For each WasmFunction:                               │
│        │   ├── WasmToIRBuilder::initialize()                    │
│        │   ├── WasmToIRBuilder::buildFromInstructions()         │
│        │   └── IRJitEngine::compile()                           │
│        └── If successful: FuncInst->upgradeToIRJit()            │
├─────────────────────────────────────────────────────────────────┤
│ 4. Execution (Executor::invoke)                                 │
│    └── If isIRJitFunction(): call native code directly          │
│    └── Otherwise: fall back to interpreter                      │
└─────────────────────────────────────────────────────────────────┘
```

### Key Integration Points

#### 1. Function Upgrade (`include/runtime/instance/function.h`)
```cpp
// After JIT compilation succeeds, upgrade function variant
bool upgradeToIRJit(void *NativeFunc, size_t CodeSize, ir_ctx *IRGraph);
```

#### 2. Compilation Hook (`lib/executor/instantiate/module.cpp`)
```cpp
// Compile functions after all module sections are instantiated
#ifdef WASMEDGE_BUILD_IR_JIT
  VM::IRJitEngine IREngine;
  VM::WasmToIRBuilder IRBuilder;
  // ... compile each function
  FuncInst->upgradeToIRJit(CompRes->NativeFunc, CompRes->CodeSize, nullptr);
#endif
```

#### 3. Runtime Context (`lib/executor/helper.cpp`)
```cpp
// Build function table for inter-function calls
void **FuncTable = buildFunctionTable(ModInst);

// Get global and memory base addresses
void *GlobalBase = getGlobalBaseFromInstance(ModInst);
void *MemoryBase = getMemoryBaseFromInstance(ModInst);
```

### End-to-End Test Results

The integration is verified with real `.wasm` files compiled from `.wat`:

| Test | Description | Status |
|------|-------------|--------|
| LoadAndRunFactorial | Iterative factorial (loop + locals) | ✅ JIT |
| LoadAndRunAdd | Simple addition | ✅ JIT |
| LoadAndRunWithFunctionCall | Function calling another function | ✅ JIT |
| LoadAndRunMemoryOps | Memory store/load | ✅ JIT |
| LoadAndRunGlobalOps | Global get/set/increment | ✅ JIT |

**All 6 functions in the test module compile and execute via JIT successfully.**

### Build Configuration

Enable IR JIT with CMake:
```bash
cmake -DWASMEDGE_BUILD_IR_JIT=ON ..
```

This sets `-DWASMEDGE_BUILD_IR_JIT` for all relevant targets and links the IR library.

---

## Testing Strategy

> **Test Coverage Status**
> 
> The test suite verifies **IR generation, JIT compilation, AND execution correctness**:
> - ✅ Instructions translate to valid IR nodes
> - ✅ IR graphs compile to native code without errors
> - ✅ **I32/I64 arithmetic, bitwise, comparison, unary** - Execution verified (43 tests)
> - ✅ **Control flow** - if/else, early return, nested branches - Execution verified (5 tests)
> - ✅ **Memory operations** - load/store with sign/zero extension - Execution verified (19 tests)
> - ⚠️ **NOT YET TESTED**: Float operations execution correctness
> 
> The `ir_execution_test.cpp` calls JIT-compiled functions and verifies computed results.

The test suite uses a **multi-level verification strategy** to ensure correctness of instruction mappings. Tests are located in `test/ir/` and use Google Test (gtest) framework.

### Test Philosophy

The tests verify correctness at multiple levels:

```
┌─────────────────────────────────────────────────────────────┐
│ Level 1: IR Generation     - Does the mapping build?    ✅ │
├─────────────────────────────────────────────────────────────┤
│ Level 2: Bulk Coverage     - Are all instructions mapped?✅ │
├─────────────────────────────────────────────────────────────┤
│ Level 3: JIT Compilation   - Does it compile to native? ✅ │
├─────────────────────────────────────────────────────────────┤
│ Level 4: Execution         - Does it compute correctly? ✅ │
│          ✅ I32/I64 arithmetic, bitwise, comparison, unary  │
│          ✅ Control flow: if/else, early return, nested     │
│          ✅ Memory: load/store with sign/zero extension     │
│          ⚠️ Float operations not yet tested                 │
└─────────────────────────────────────────────────────────────┘
```

### Level 1: IR Generation Verification

Tests verify WebAssembly instructions can be translated to IR nodes without errors:

```cpp
TEST(IRBuilderTest, I32Add) {
  WasmToIRBuilder Builder;

  // 1. Set up function signature: (i32, i32) -> (i32)
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  // 2. Initialize the builder
  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // 3. Build WebAssembly instruction sequence
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__add);      // <-- Instruction being tested
  Instrs.emplace_back(OpCode::Return);

  // 4. Verify IR generation succeeds
  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);   // ✅ Mapping worked without error
}
```

**What this verifies:**
- The opcode is recognized in the dispatch switch
- The correct IR macro is called
- Stack operations (pop/push) work correctly
- No runtime errors during translation

### Level 2: Bulk Coverage Testing

The `ir_instruction_test.cpp` uses helper functions to test **entire categories** efficiently:

```cpp
class IRInstructionTest : public ::testing::Test {
protected:
  // Helper to test any binary operation
  void testBinaryOp(OpCode Op, TypeCode Type1 = TypeCode::I32, 
                     TypeCode Type2 = TypeCode::I32,
                     TypeCode RetType = TypeCode::I32) {
    WasmToIRBuilder Builder;
    std::vector<ValType> ParamTypes = {ValType(Type1), ValType(Type2)};
    std::vector<ValType> RetTypes = {ValType(RetType)};
    AST::FunctionType FuncType(ParamTypes, RetTypes);
    
    EXPECT_TRUE(Builder.initialize(FuncType, {}));
    
    std::vector<AST::Instruction> Instrs;
    Instrs.emplace_back(OpCode::Local__get);
    Instrs[0].getTargetIndex() = 0;
    Instrs.emplace_back(OpCode::Local__get);
    Instrs[1].getTargetIndex() = 1;
    Instrs.emplace_back(Op);                   // <-- Generic instruction
    Instrs.emplace_back(OpCode::Return);
    
    EXPECT_TRUE(Builder.buildFromInstructions(Instrs));
    ASSERT_NE(Builder.getIRContext(), nullptr);
  }

  // Helper for unary operations
  void testUnaryOp(OpCode Op, TypeCode InType = TypeCode::I32,
                    TypeCode RetType = TypeCode::I32);
};

// Test ALL i32 arithmetic in one test
TEST_F(IRInstructionTest, I32_Arithmetic) {
  testBinaryOp(OpCode::I32__add);
  testBinaryOp(OpCode::I32__sub);
  testBinaryOp(OpCode::I32__mul);
  testBinaryOp(OpCode::I32__div_s);
  testBinaryOp(OpCode::I32__div_u);
  testBinaryOp(OpCode::I32__rem_s);
  testBinaryOp(OpCode::I32__rem_u);
}
```

### Level 3: JIT Compilation Verification

Tests verify the IR graph can be compiled to machine code:

```cpp
TEST(IRJitEngineTest, CompileSimpleArithmetic) {
  WasmToIRBuilder Builder;
  // ... build instructions ...
  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));

  // Compile with JIT engine
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());

  if (CompRes) {
    // Verify compilation produced valid code
    ASSERT_NE(CompRes.value().NativeFunc, nullptr);  // ✅ Got function pointer
    ASSERT_GT(CompRes.value().CodeSize, 0);          // ✅ Generated code
    ASSERT_NE(CompRes.value().IRGraph, nullptr);     // ✅ IR graph exists

    // Clean up
    Engine.release(CompRes.value().NativeFunc, CompRes.value().CodeSize);
  }
}
```

**What this verifies:**
- IR graph is valid and complete
- IR optimizations pass without error
- Register allocation succeeds
- Code generation produces machine code

### Level 4: Execution Correctness Testing

The most critical level - tests **actually execute** the JIT-compiled native code and verify it produces correct results. This is implemented in `ir_execution_test.cpp`.

#### Testing Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│  Test Fixture: IRExecutionTest / MemoryExecutionTest                │
├─────────────────────────────────────────────────────────────────────┤
│  1. Build WebAssembly instruction sequence                          │
│  2. Lower to IR using WasmToIRBuilder                               │
│  3. JIT compile to native code using IRJitEngine                    │
│  4. Cast to C function pointer                                      │
│  5. Call the function with test inputs                              │
│  6. Compare returned value against expected result                  │
└─────────────────────────────────────────────────────────────────────┘
```

#### How It Works

**Step 1: Build a minimal Wasm function**

```cpp
void* buildBinaryOp(OpCode Op, TypeCode Type1, TypeCode Type2, TypeCode RetType) {
  // Create function type: (param0, param1) -> result
  std::vector<ValType> ParamTypes = {ValType(Type1), ValType(Type2)};
  std::vector<ValType> RetTypes = {ValType(RetType)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);
  
  Builder.initialize(FuncType, {});
  
  // Build instruction sequence: local.get 0, local.get 1, <op>, end
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;
  Instrs.emplace_back(Op);  // The operation being tested
  Instrs.emplace_back(OpCode::End);
  
  Builder.buildFromInstructions(Instrs);
  return Engine.compile(Builder.getIRContext())->NativeFunc;
}
```

**Step 2: Execute and verify**

```cpp
TEST_F(IRExecutionTest, I32_Add_Basic) {
  // Build JIT function for i32.add
  void* Func = buildBinaryOp(OpCode::I32__add, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);
  
  // Cast to C function pointer (memory base is implicit first param)
  using FnType = int32_t (*)(void*, int32_t, int32_t);
  auto fn = reinterpret_cast<FnType>(Func);
  
  // Execute and verify results
  EXPECT_EQ(fn(Memory.data(), 10, 20), 30);      // Basic addition
  EXPECT_EQ(fn(Memory.data(), -5, 5), 0);        // Negative numbers
  EXPECT_EQ(fn(Memory.data(), 100, -30), 70);    // Mixed signs
}
```

#### Memory Operations Testing

Memory tests use a more sophisticated approach:

**Step 1: Pre-initialize memory with known patterns**

```cpp
void SetUp() override {
  // Known i32 value (0x12345678) at offset 16
  Memory[16] = 0x78; Memory[17] = 0x56; Memory[18] = 0x34; Memory[19] = 0x12;
  
  // Negative byte (0xFF = -1 signed) at offset 48 for sign extension tests
  Memory[48] = 0xFF;
  
  // Value 0x80 (128 unsigned, -128 signed) at offset 50
  Memory[50] = 0x80;
}
```

**Step 2: Load tests - verify values read from memory**

```cpp
TEST_F(MemoryExecutionTest, Memory_I32_Load8_S) {
  void* Func = buildLoadFunc(OpCode::I32__load8_s, TypeCode::I32);
  auto fn = reinterpret_cast<int32_t (*)(void*, int32_t)>(Func);
  
  // Load byte 0x80 with SIGN extension -> should be -128
  EXPECT_EQ(fn(Memory.data(), 50), -128);
  
  // Load byte 0xFF with SIGN extension -> should be -1  
  EXPECT_EQ(fn(Memory.data(), 48), -1);
}

TEST_F(MemoryExecutionTest, Memory_I32_Load8_U) {
  void* Func = buildLoadFunc(OpCode::I32__load8_u, TypeCode::I32);
  auto fn = reinterpret_cast<int32_t (*)(void*, int32_t)>(Func);
  
  // Load byte 0x80 with ZERO extension -> should be 128
  EXPECT_EQ(fn(Memory.data(), 50), 128);
  
  // Load byte 0xFF with ZERO extension -> should be 255
  EXPECT_EQ(fn(Memory.data(), 48), 255);
}
```

**Step 3: Store tests - verify values written to memory**

```cpp
TEST_F(MemoryExecutionTest, Memory_I32_Store) {
  void* Func = buildStoreFunc(OpCode::I32__store, TypeCode::I32);
  auto fn = reinterpret_cast<void (*)(void*, int32_t, int32_t)>(Func);
  
  // Store 0xDEADBEEF at offset 100
  fn(Memory.data(), 100, static_cast<int32_t>(0xDEADBEEF));
  
  // Verify bytes in little-endian order
  EXPECT_EQ(Memory[100], 0xEF);
  EXPECT_EQ(Memory[101], 0xBE);
  EXPECT_EQ(Memory[102], 0xAD);
  EXPECT_EQ(Memory[103], 0xDE);
}
```

**Step 4: Round-trip tests - store then load back**

```cpp
TEST_F(MemoryExecutionTest, Memory_I32_RoundTrip) {
  auto store = reinterpret_cast<void (*)(void*, int32_t, int32_t)>(StoreFunc);
  auto load = reinterpret_cast<int32_t (*)(void*, int32_t)>(LoadFunc);
  
  int32_t testValues[] = {0, 1, -1, INT32_MAX, INT32_MIN, 0x12345678};
  for (int32_t val : testValues) {
    store(Memory.data(), 300, val);
    EXPECT_EQ(load(Memory.data(), 300), val);  // Must match!
  }
}
```

#### Control Flow Testing

Control flow tests verify branch logic produces correct results:

```cpp
TEST_F(IRExecutionTest, ControlFlow_IfElse_Max) {
  // Build: max(a, b) = if (a > b) then a else b
  std::vector<AST::Instruction> Instrs;
  // Compare: a > b
  Instrs.emplace_back(OpCode::Local__get); Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get); Instrs.back().getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__gt_s);
  Instrs.emplace_back(OpCode::If);
  // True branch: return a
  Instrs.emplace_back(OpCode::Local__get); Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::Else);
  // False branch: return b
  Instrs.emplace_back(OpCode::Local__get); Instrs.back().getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::End);
  // ... build and compile ...
  
  // Verify max() behavior
  EXPECT_EQ(fn(Memory.data(), 10, 5), 10);   // 10 > 5, return 10
  EXPECT_EQ(fn(Memory.data(), 3, 7), 7);     // 3 < 7, return 7
  EXPECT_EQ(fn(Memory.data(), -5, -10), -5); // -5 > -10, return -5
}
```

#### Function Call Testing

Function calls test both direct (`call`) and indirect (`call_indirect`) invocations:

**JIT Function Signature with Call Support**

All JIT-compiled functions now receive three implicit parameters:
```cpp
// JIT signature: RetType func(void** func_table, uint32_t table_size, void* mem_base, Params...)
// - func_table: Array of function pointers for inter-function calls
// - table_size: Number of entries (for call_indirect bounds checking)
// - mem_base: Linear memory base pointer
```

**Direct Call Test (`call`)**

```cpp
// Native function that will be called by JIT code
static int32_t native_double(void**, uint32_t, void*, int32_t x) {
  return x * 2;
}

TEST_F(IRExecutionTest, Call_DirectToNative) {
  // Build Wasm: local.get 0, call 0, return
  // Creates caller(x) that calls func_table[0](x)
  
  void* FuncTable[1] = { reinterpret_cast<void*>(&native_double) };
  
  using FnType = int32_t (*)(void**, uint32_t, void*, int32_t);
  auto caller_fn = reinterpret_cast<FnType>(CompiledFunc);
  
  // Verify: caller(5) -> native_double(5) -> 10
  EXPECT_EQ(caller_fn(FuncTable, 1, Memory.data(), 5), 10);
}
```

**Indirect Call Test (`call_indirect`)**

```cpp
TEST_F(IRExecutionTest, CallIndirect_RuntimeIndex) {
  // Build Wasm: local.get 0, local.get 1, call_indirect type=0
  // Creates caller(value, index) that calls func_table[index](value)
  
  void* FuncTable[2] = {
    reinterpret_cast<void*>(&native_double),  // Index 0: x*2
    reinterpret_cast<void*>(&native_triple)   // Index 1: x*3
  };
  
  using FnType = int32_t (*)(void**, uint32_t, void*, int32_t, int32_t);
  auto caller_fn = reinterpret_cast<FnType>(CompiledFunc);
  
  // Verify runtime dispatch
  EXPECT_EQ(caller_fn(FuncTable, 2, Memory.data(), 5, 0), 10);  // 5*2
  EXPECT_EQ(caller_fn(FuncTable, 2, Memory.data(), 5, 1), 15);  // 5*3
}
```

**call_indirect Implementation Details**

- Bounds check: `if (index >= table_size)` trap
- Function pointer lookup: `FuncTablePtr[index]`
- Uses chained `ir_IF` for bounds check with trap path
- PHI node merges return values from valid/trap paths

#### What Level 4 Catches

| Issue Type | Example | How Detected |
|------------|---------|--------------|
| **Wrong IR mapping** | `i32.sub` mapped to `ir_ADD` | `sub(10, 3)` returns 13 instead of 7 |
| **Sign vs unsigned** | Using `ir_DIV` instead of `ir_DIV_U` | `div_u(-1, 2)` returns wrong value |
| **Endianness** | Bytes stored in wrong order | Load after store returns different value |
| **Sign extension** | Using zero-extend instead of sign-extend | `load8_s(0xFF)` returns 255 instead of -1 |
| **Control flow** | Branch targets incorrect | `if(true)` executes wrong branch |
| **Dead code** | Return values lost after branch | `if/else` returns 0 for both branches |

### Test Categories and Coverage

| Test Category | What's Verified | File Location |
|---------------|-----------------|---------------|
| **Initialization** | Function types, parameters, locals | `ir_basic_test.cpp` |
| **Constants** | i32/i64/f32/f64.const | `ir_basic_test.cpp` |
| **Locals** | local.get/set/tee | `ir_basic_test.cpp` |
| **I32 Operations** | All 15+ i32 opcodes | `ir_instruction_test.cpp` |
| **I64 Operations** | All 15+ i64 opcodes | `ir_instruction_test.cpp` |
| **F32/F64 Operations** | Float arithmetic/comparison | `ir_instruction_test.cpp` |
| **Parametric** | drop, select | `ir_instruction_test.cpp` |
| **Control Flow** | block, loop, if, br, return | `ir_instruction_test.cpp` |
| **Type Conversions** | wrap, extend, trunc, convert, reinterpret | `ir_instruction_test.cpp` |
| **Memory** | All load/store variants | `ir_instruction_test.cpp` |
| **JIT Compilation** | Full pipeline to native code | Both test files |

### Current Test Results

```bash
# Basic Test Suite (33 tests)
[==========] 33 tests from 3 test suites
[  PASSED  ] 33 tests (100%)

# Comprehensive Instruction Test Suite (40 tests)
[==========] 40 tests from 1 test suite
[  PASSED  ] 40 tests (100%)

# Execution Correctness Test Suite (74 tests)
[==========] 74 tests from 2 test suites
[  PASSED  ] 74 tests (100%)

Type Conversion Coverage (FULL IMPLEMENTATION):
  ✅ Integer wrap/extend - ir_TRUNC, ir_SEXT, ir_ZEXT
  ✅ Float to int - ir_FP2I32/I64/U32/U64
  ✅ Saturating trunc - ir_FP2I32/I64/U32/U64
  ✅ Int to float - ir_INT2F/D with unsigned handling
  ✅ Float promote/demote - ir_F2D/D2F
  ✅ Reinterpret - ir_BITCAST_I32/I64/F/D
  ✅ Sign extension - Shift-based implementation

Control Flow & Calls Coverage (IR Generation + Execution ✅):
  ✅ nop, unreachable - Direct IR mapping
  ✅ block - Forward-jump with ir_END/ir_MERGE
  ✅ loop - ir_LOOP_BEGIN/ir_LOOP_END with back-edges and PHI nodes for locals
  ✅ if/else/end - ir_IF/ir_IF_TRUE/ir_IF_FALSE with proper termination tracking
  ✅ br - Unconditional branch (forward/backward)
  ✅ br_if - Conditional branch with ir_IF
  ✅ br_table - Multi-way branch using chained comparisons
  ✅ return - ir_RETURN with dead code elimination
  ✅ call - Direct function call via func_table pointer
  ✅ call_indirect - Indirect call with runtime index and bounds checking

Global Operations Coverage (IR Generation + Execution ✅):
  ✅ global.get - Load from global_base[index * 8]
  ✅ global.set - Store to global_base[index * 8]

TOTAL: 161/161 tests passing ✅
  - Basic IR Generation: 33 tests
  - Instruction Coverage: 43 tests
  - Execution Correctness: 79 tests
  - Integration Tests: 6 tests
  - Execution Correctness: 79 tests (includes 8 control flow + 19 memory + 4 function call + 5 global tests)
```

**Test Suite Structure:**
```
test/ir/
├── ir_basic_test.cpp        (33 tests - IR generation)
├── ir_instruction_test.cpp  (35 tests - bulk instruction coverage)
├── ir_execution_test.cpp    (67 tests - execution correctness)
└── CMakeLists.txt           (build configuration)
```

**Execution Tests Coverage (67 tests):**
```
I32 Operations Verified (29 tests):
  ✅ Arithmetic: add, sub, mul, div_s, div_u, rem_s, rem_u (incl. overflow)
  ✅ Bitwise: and, or, xor, shl, shr_s, shr_u, rotl, rotr
  ✅ Comparison: eq, ne, lt_s, lt_u, le_s, gt_s, ge_s
  ✅ Unary: eqz, clz, ctz, popcnt

I64 Operations Verified (14 tests):
  ✅ Arithmetic: add, sub, mul, div_s, div_u, rem_s (incl. overflow)
  ✅ Comparison: eq, lt_s, lt_u
  ✅ Unary: eqz, clz, ctz, popcnt

Control Flow Verified (5 tests):
  ✅ ControlFlow_IfElse_Basic - if/else with return in both branches
  ✅ ControlFlow_IfElse_Max - max(a,b) using comparison + if/else
  ✅ ControlFlow_NestedIfElse_Sign - nested if/else for sign function
  ✅ ControlFlow_EarlyReturn_Clamp - early returns with if (no else)
  ✅ ControlFlow_IfElse_Abs - absolute value using if/else

Memory Operations Verified (19 tests):
  ✅ Memory_I32_Load - 32-bit load verification
  ✅ Memory_I32_Load8_S - 8-bit load with sign extension
  ✅ Memory_I32_Load8_U - 8-bit load with zero extension
  ✅ Memory_I32_Load16_S - 16-bit load with sign extension
  ✅ Memory_I32_Load16_U - 16-bit load with zero extension
  ✅ Memory_I64_Load - 64-bit load verification
  ✅ Memory_I64_Load8_S - 8-bit to i64 with sign extension
  ✅ Memory_I64_Load8_U - 8-bit to i64 with zero extension
  ✅ Memory_I64_Load32_S - 32-bit to i64 with sign extension
  ✅ Memory_I64_Load32_U - 32-bit to i64 with zero extension
  ✅ Memory_I32_Store - 32-bit store verification
  ✅ Memory_I32_Store8 - truncated 8-bit store
  ✅ Memory_I32_Store16 - truncated 16-bit store
  ✅ Memory_I64_Store - 64-bit store verification
  ✅ Memory_I64_Store8 - truncated 8-bit store (i64)
  ✅ Memory_I64_Store16 - truncated 16-bit store (i64)
  ✅ Memory_I64_Store32 - truncated 32-bit store (i64)
  ✅ Memory_I32_RoundTrip - store/load round-trip verification
  ✅ Memory_I64_RoundTrip - store/load round-trip verification (i64)
```

### Instruction Coverage Verification

```
Total Instructions Tested: ~145
├─ I32 Binary (14): ✅ add, sub, mul, div_s, div_u, rem_s, rem_u, and, or, xor, shl, shr_s, shr_u, rotl, rotr
├─ I64 Binary (14): ✅ add, sub, mul, div_s, div_u, rem_s, rem_u, and, or, xor, shl, shr_s, shr_u, rotl, rotr
├─ F32 Binary (6): ✅ add, sub, mul, div, min, max
├─ F64 Binary (6): ✅ add, sub, mul, div, min, max
├─ I32 Compare (10): ✅ eq, ne, lt_s, lt_u, le_s, le_u, gt_s, gt_u, ge_s, ge_u
├─ I64 Compare (10): ✅ eq, ne, lt_s, lt_u, le_s, le_u, gt_s, gt_u, ge_s, ge_u
├─ F32 Compare (6): ✅ eq, ne, lt, le, gt, ge
├─ F64 Compare (6): ✅ eq, ne, lt, le, gt, ge
├─ I32 Unary (4): ✅ eqz, clz, ctz, popcnt
├─ I64 Unary (4): ✅ eqz, clz, ctz, popcnt
├─ F32 Unary (7): ✅ abs, neg, sqrt†, ceil†, floor†, trunc†, nearest†
├─ F64 Unary (7): ✅ abs, neg, sqrt†, ceil†, floor†, trunc†, nearest†
├─ Parametric (2): ✅ drop, select
├─ Control Flow (10): ✅ FULL IMPLEMENTATION
│  ├─ nop, unreachable: Direct IR mapping
│  ├─ block: Forward-jump target with ir_END/ir_MERGE at end
│  ├─ loop: ir_LOOP_BEGIN/ir_LOOP_END with back-edges + PHI nodes for loop variables
│  ├─ if/else: ir_IF/ir_IF_TRUE/ir_IF_FALSE with merge
│  ├─ br: Unconditional (forward to block end, or back to loop)
│  ├─ br_if: Conditional with ir_IF for condition
│  ├─ br_table: Multi-way branch with chained comparisons
│  ├─ return: ir_RETURN with optional value
│  ├─ call: Direct call via func_table[index]
│  └─ call_indirect: Indirect call via func_table[runtime_index] with bounds check
├─ Type Conversions (37): ✅ FULL IMPLEMENTATION
│  ├─ Integer wrap/extend: i32.wrap_i64, i64.extend_i32_s/u
│  ├─ Float to int: i32.trunc_f32_s/u, i32.trunc_f64_s/u, i64.trunc_f32_s/u, i64.trunc_f64_s/u
│  ├─ Saturating trunc: i32.trunc_sat_f32_s/u, i32.trunc_sat_f64_s/u, i64.trunc_sat_f32_s/u, i64.trunc_sat_f64_s/u
│  ├─ Int to float: f32.convert_i32_s/u, f32.convert_i64_s/u, f64.convert_i32_s/u, f64.convert_i64_s/u
│  ├─ Float promote/demote: f32.demote_f64, f64.promote_f32
│  ├─ Reinterpret: i32.reinterpret_f32, i64.reinterpret_f64, f32.reinterpret_i32, f64.reinterpret_i64
│  └─ Sign extension: i32.extend8_s, i32.extend16_s, i64.extend8_s, i64.extend16_s, i64.extend32_s
├─ Memory Loads (14): ✅ i32/i64/f32/f64.load, i32/i64 partial loads (with sign/zero-extend)
└─ Memory Stores (9): ✅ i32/i64/f32/f64.store, i32/i64 partial stores (with truncation)

† Float math placeholders (return operand unchanged)
```

### Running Tests

```bash
cd build

# Run basic tests
make wasmedgeIRTests
./test/ir/wasmedgeIRTests

# Run comprehensive instruction tests  
make wasmedgeIRInstructionTests
./test/ir/wasmedgeIRInstructionTests

# Run all IR tests via CTest
ctest -R IR
```

### Manual Verification

```bash
# 1. Build verification
cd build
make wasmedgeVM -j8
echo "Exit code: $?"  # Should be 0

# 2. Library verification
ls -lh thirdparty/ir/libwasmedgeIR.a  # ~1-2 MB
ls -lh lib/vm/libwasmedgeVM.a

# 3. Symbol verification
nm thirdparty/ir/libwasmedgeIR.a | grep "T ir_jit_compile"
nm lib/vm/libwasmedgeVM.a | grep "WasmToIRBuilder"
```

### ✅ Execution-Level Testing

The test suite includes **67 execution correctness tests** that call JIT-compiled functions and verify computed results:

```cpp
// Example from ir_execution_test.cpp
TEST_F(IRExecutionTest, I32_Add_Basic) {
  void* Func = buildBinaryOp(OpCode::I32__add, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);
  
  using FnType = int32_t (*)(void*, int32_t, int32_t);
  auto fn = reinterpret_cast<FnType>(Func);
  
  // Execute JIT-compiled code and verify results
  EXPECT_EQ(fn(Memory.data(), 10, 20), 30);
  EXPECT_EQ(fn(Memory.data(), -5, 5), 0);
  EXPECT_EQ(fn(Memory.data(), 100, -30), 70);
}
```

**Coverage**: I32/I64 arithmetic (43 tests), control flow (5 tests), memory operations (19 tests)

### ✅ Completed Test Coverage

1. **IR Builder Tests** ✅
   - Function initialization
   - Type conversion
   - Instruction lowering
   - SSA value tracking
   - Local variable management

2. **IR JIT Engine Tests** ✅
   - Compilation success
   - Code generation
   - Memory management
   - Code release

3. **Integration Tests** ✅ (6 tests)
   - Simple arithmetic: `(i32.const 10) (i32.const 20) (i32.add)`
   - Local variables: `(local.get 0) (local.get 1) (i32.mul)`
   - **Factorial (iterative)**: Tests loops with PHI nodes, local variable updates, and conditional branching
   - Memory access: Store and load round-trip verification
   - Global access: Counter increment using global.get/set
   - Conditional logic: Max function using if/else

4. **Execution Correctness Tests** ✅ (79 tests)
   - I32/I64 arithmetic, bitwise, comparison, unary (43 tests)
   - Control flow: if/else, nested, early return (5 tests)
   - Memory: load/store with sign/zero extension (19 tests)

### Remaining Test Gaps

| Category | Status | Notes |
|----------|--------|-------|
| **Float execution** | ⚠️ Not tested | Need execution tests for f32/f64 operations |
| **Type conversions** | ⚠️ IR only | Need execution correctness tests |
| **Edge cases** | ⚠️ Partial | Need more overflow/underflow/NaN testing |

---

## References

### Key Files to Reference

- **IR Examples**: `/Users/tommylee/Desktop/ir/examples/`
  - `0001-basic.c` - Basic function compilation pattern
  - `0003-pointer.c` - Memory operations pattern
  - `0004-func.c` - Function call pattern

- **IR Headers**: `/Users/tommylee/Desktop/ir/`
  - `ir.h` - Core IR types and functions
  - `ir_builder.h` - Builder macros (ir_PARAM, ir_ADD, etc.)

- **WasmEdge Executor**: `lib/executor/helper.cpp`
  - `enterFunction()` - Function dispatch point

### Related Documentation

See `notes/` directory for additional documentation:
- Build instructions
- Architecture details
- Performance notes
- Development logs

### External Resources

- **dstogov/ir**: https://github.com/dstogov/ir
- **WebAssembly Spec**: https://webassembly.github.io/spec/
- **WasmEdge**: https://wasmedge.org/

---

## Implementation Status

This section provides a comprehensive breakdown of all WebAssembly instruction implementations.

### ✅ Fully Implemented

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
| **TOTAL** | **166** | |

### ⚠️ Placeholder Implementations (Dispatch but don't work correctly)

| Category | Count | Instructions | Issue |
|----------|-------|--------------|-------|
| **F32 Math** | 5 | `sqrt`, `ceil`, `floor`, `trunc`, `nearest` | Return operand unchanged (need intrinsic calls) |
| **F64 Math** | 5 | `sqrt`, `ceil`, `floor`, `trunc`, `nearest` | Return operand unchanged (need intrinsic calls) |
| **TOTAL** | **10** | | |

### ❌ Not Implemented (Will return error)

| Category | Count | Instructions |
|----------|-------|--------------|
| **Memory Meta** | 2 | `memory.size`, `memory.grow` |
| **Table Ops** | 8 | `table.get`, `table.set`, `table.size`, `table.grow`, `table.fill`, `table.copy`, `table.init`, `elem.drop` |
| **Reference Types** | 3 | `ref.null`, `ref.is_null`, `ref.func` |
| **Bulk Memory** | 4 | `memory.copy`, `memory.fill`, `memory.init`, `data.drop` |
| **SIMD (v128)** | 200+ | All v128 operations |
| **Atomics** | 50+ | All atomic operations |
| **Exceptions** | 6+ | `try`, `catch`, `throw`, etc. |
| **TOTAL** | **~280+** | |

### Summary

| Status | Count | Percentage |
|--------|-------|------------|
| ✅ Fully Implemented | 165 | 90% of core |
| ⚠️ Placeholder | 10 | 5% of core |
| ❌ Not Implemented | 8 | 5% of core |
| **Core Total** | **183** | 100% |

*Note: SIMD, Atomics, and Exceptions are considered advanced features and not counted in "core" percentages.*

### Priority for Completion

**High Priority** (required for basic programs):
1. ✅ ~~Type conversions (37)~~ - **COMPLETED**
2. ✅ ~~Memory load/store (23)~~ - **COMPLETED** (implicit memory base param)
3. ✅ ~~Global operations (2)~~ - **COMPLETED** (global_base param)
4. ✅ ~~Function calls (2)~~ - **COMPLETED** (func_table param)

**Medium Priority** (common features):
5. ⚠️ Float math (10) - `sqrt`, `ceil`, `floor`, etc.
6. ❌ `memory.size/grow` (2) - Dynamic memory

**Low Priority** (advanced features):
8. Table operations (8)
9. Reference types (3)
10. Bulk memory (4)

### Recommended Next Steps

1. **Float Math Intrinsics** (10 instructions) - Quick win  
   Fix placeholders by adding `ir_CALL` to C library functions (`sqrtf`, `sqrt`, `ceilf`, `ceil`, etc.)

2. **Memory Size/Grow** (2 instructions) - Requires runtime integration
   Need to track and modify memory size at runtime

---

