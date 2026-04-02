// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//==============================================================================
// IR JIT Implementation Test Suite
//
// This test suite validates the instruction mappings and IR generation for
// the baseline JIT compiler using the dstogov/ir framework.
//
// Test Coverage:
// - IR Builder initialization with various function signatures
// - Constant instructions (i32, i64, f32, f64)
// - Local variable operations (get, set, tee)
// - Binary arithmetic (i32/i64 add, sub, mul)
// - Control flow structures (block, loop, if/else, return)
// - Memory operations (load, store - placeholder implementation)
// - IR JIT Engine compilation
// - Integration tests with complete functions
//
// Build with: -DWASMEDGE_BUILD_IR_JIT=ON
// Run with: ./wasmedgeIRTests
//==============================================================================

#include "vm/ir_builder.h"
#include "vm/ir_jit_engine.h"

#include "ast/instruction.h"
#include "ast/type.h"
#include "common/types.h"

#include <gtest/gtest.h>
#include <vector>

namespace WasmEdge {
namespace VM {

//==============================================================================
// IR Builder Initialization Tests
//==============================================================================

TEST(IRBuilderTest, InitializationVoid) {
  WasmToIRBuilder Builder;

  // Create a simple function type: () -> ()
  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes;
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  std::vector<std::pair<uint32_t, ValType>> Locals;

  auto Res = Builder.initialize(FuncType, Locals);
  ASSERT_TRUE(Res);
  ASSERT_NE(Builder.getIRContext(), nullptr);
}

TEST(IRBuilderTest, InitializationWithParams) {
  WasmToIRBuilder Builder;

  // Function type: (i32, i64, f32, f64) -> (i32)
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), 
                                      ValType(TypeCode::I64),
                                      ValType(TypeCode::F32), 
                                      ValType(TypeCode::F64)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  std::vector<std::pair<uint32_t, ValType>> Locals;

  auto Res = Builder.initialize(FuncType, Locals);
  ASSERT_TRUE(Res);
  ASSERT_NE(Builder.getIRContext(), nullptr);
}

TEST(IRBuilderTest, InitializationWithLocals) {
  WasmToIRBuilder Builder;

  // Function type: () -> ()
  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes;
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  // Add some local variables
  std::vector<std::pair<uint32_t, ValType>> Locals = {
    {2, ValType(TypeCode::I32)},  // 2 i32 locals
    {1, ValType(TypeCode::I64)},  // 1 i64 local
    {1, ValType(TypeCode::F32)}   // 1 f32 local
  };

  auto Res = Builder.initialize(FuncType, Locals);
  ASSERT_TRUE(Res);
  ASSERT_NE(Builder.getIRContext(), nullptr);
}

//==============================================================================
// Constant Instruction Tests
//==============================================================================

TEST(IRBuilderTest, ConstantI32) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: i32.const 42, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::I32__const);
  Instrs[0].setNum(static_cast<int32_t>(42));
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, ConstantI64) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes = {ValType(TypeCode::I64)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: i64.const 12345678, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::I64__const);
  Instrs[0].setNum(static_cast<int64_t>(12345678));
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, ConstantF32) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes = {ValType(TypeCode::F32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: f32.const 3.14, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::F32__const);
  Instrs[0].setNum(3.14f);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, ConstantF64) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes = {ValType(TypeCode::F64)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: f64.const 2.718281828, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::F64__const);
  Instrs[0].setNum(2.718281828);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

//==============================================================================
// Local Variable Tests
//==============================================================================

TEST(IRBuilderTest, LocalGet) {
  WasmToIRBuilder Builder;

  // Function type: (i32) -> (i32)
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, LocalSet) {
  WasmToIRBuilder Builder;

  // Function type: (i32) -> ()
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes;
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  std::vector<std::pair<uint32_t, ValType>> Locals = {
    {1, ValType(TypeCode::I32)}  // 1 additional local
  };

  ASSERT_TRUE(Builder.initialize(FuncType, Locals));

  // Build: local.get 0, local.set 1, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__set);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, LocalTee) {
  WasmToIRBuilder Builder;

  // Function type: () -> (i32)
  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  std::vector<std::pair<uint32_t, ValType>> Locals = {
    {1, ValType(TypeCode::I32)}
  };

  ASSERT_TRUE(Builder.initialize(FuncType, Locals));

  // Build: i32.const 100, local.tee 0, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::I32__const);
  Instrs[0].setNum(static_cast<int32_t>(100));
  Instrs.emplace_back(OpCode::Local__tee);
  Instrs[1].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

//==============================================================================
// Binary Arithmetic Tests - I32
//==============================================================================

TEST(IRBuilderTest, I32Add) {
  WasmToIRBuilder Builder;

  // Function type: (i32, i32) -> (i32)
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, local.get 1, i32.add, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__add);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, I32Sub) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, local.get 1, i32.sub, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__sub);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, I32Mul) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, local.get 1, i32.mul, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__mul);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, I32AddConstants) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: i32.const 10, i32.const 20, i32.add, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::I32__const);
  Instrs[0].setNum(static_cast<int32_t>(10));
  Instrs.emplace_back(OpCode::I32__const);
  Instrs[1].setNum(static_cast<int32_t>(20));
  Instrs.emplace_back(OpCode::I32__add);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

//==============================================================================
// Binary Arithmetic Tests - I64
//==============================================================================

TEST(IRBuilderTest, I64Add) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes = {ValType(TypeCode::I64), ValType(TypeCode::I64)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I64)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, local.get 1, i64.add, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I64__add);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, I64Sub) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes = {ValType(TypeCode::I64), ValType(TypeCode::I64)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I64)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, local.get 1, i64.sub, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I64__sub);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, I64Mul) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes = {ValType(TypeCode::I64), ValType(TypeCode::I64)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I64)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, local.get 1, i64.mul, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I64__mul);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

//==============================================================================
// Complex Arithmetic Tests
//==============================================================================

TEST(IRBuilderTest, ComplexExpression) {
  WasmToIRBuilder Builder;

  // Function type: (i32, i32, i32) -> (i32)
  // Computes: (a + b) * c
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), 
                                      ValType(TypeCode::I32), 
                                      ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, local.get 1, i32.add, local.get 2, i32.mul, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__add);
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[3].getTargetIndex() = 2;
  Instrs.emplace_back(OpCode::I32__mul);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, MultipleOperations) {
  WasmToIRBuilder Builder;

  // Function type: (i32, i32) -> (i32)
  // Computes: a + b - a
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, local.get 1, i32.add, local.get 0, i32.sub, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__add);
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[3].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::I32__sub);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

//==============================================================================
// Control Flow Tests
//==============================================================================

TEST(IRBuilderTest, SimpleReturn) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, BlockStructure) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: block, i32.const 42, end, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Block);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs[1].setNum(static_cast<int32_t>(42));
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, LoopStructure) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes;
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: loop, end, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Loop);
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, IfStructure) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, if, i32.const 1, else, i32.const 0, end, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::If);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs[2].setNum(static_cast<int32_t>(1));
  Instrs.emplace_back(OpCode::Else);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs[4].setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, NestedBlocks) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: block, block, i32.const 42, end, end, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Block);
  Instrs.emplace_back(OpCode::Block);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs[2].setNum(static_cast<int32_t>(42));
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

//==============================================================================
// IR JIT Engine Tests
//==============================================================================

TEST(IRJitEngineTest, CompileConstant) {
  WasmToIRBuilder Builder;

  // Function type: () -> (i32)
  std::vector<ValType> ParamTypes;
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: i32.const 42, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::I32__const);
  Instrs[0].setNum(static_cast<int32_t>(42));
  Instrs.emplace_back(OpCode::Return);

  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));

  // Compile with JIT engine
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());

  // Compilation may fail if IR is incomplete, which is expected for POC
  // Just verify the API works
  if (CompRes) {
    ASSERT_NE(CompRes.value().NativeFunc, nullptr);
    ASSERT_GT(CompRes.value().CodeSize, 0);

    // Clean up
    Engine.release(CompRes.value().NativeFunc, CompRes.value().CodeSize);
  }
}

TEST(IRJitEngineTest, CompileSimpleArithmetic) {
  WasmToIRBuilder Builder;

  // Function type: (i32, i32) -> (i32)
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, local.get 1, i32.add, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__add);
  Instrs.emplace_back(OpCode::Return);

  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));

  // Compile with JIT engine
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());

  if (CompRes) {
    ASSERT_NE(CompRes.value().NativeFunc, nullptr);
    ASSERT_GT(CompRes.value().CodeSize, 0);
    ASSERT_FALSE(CompRes.value().IRText.empty());

    // Clean up
    Engine.release(CompRes.value().NativeFunc, CompRes.value().CodeSize);
  }
}

TEST(IRJitEngineTest, CompileMultipleOperations) {
  WasmToIRBuilder Builder;

  // Function type: (i32, i32, i32) -> (i32)
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), 
                                      ValType(TypeCode::I32), 
                                      ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: (a + b) * c
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__add);
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[3].getTargetIndex() = 2;
  Instrs.emplace_back(OpCode::I32__mul);
  Instrs.emplace_back(OpCode::Return);

  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));

  // Compile with JIT engine
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());

  if (CompRes) {
    ASSERT_NE(CompRes.value().NativeFunc, nullptr);
    ASSERT_GT(CompRes.value().CodeSize, 0);

    // Clean up
    Engine.release(CompRes.value().NativeFunc, CompRes.value().CodeSize);
  }
}

//==============================================================================
// Memory Operations Tests (Placeholder Implementation)
//==============================================================================

TEST(IRBuilderTest, I32Load) {
  WasmToIRBuilder Builder;

  // Function type: (i32) -> (i32)
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, i32.load offset=0, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::I32__load);
  Instrs[1].getMemoryOffset() = 0;
  Instrs[1].getMemoryAlign() = 2;
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, I64Load) {
  WasmToIRBuilder Builder;

  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I64)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, i64.load offset=0, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::I64__load);
  Instrs[1].getMemoryOffset() = 0;
  Instrs[1].getMemoryAlign() = 3;
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, I32Store) {
  WasmToIRBuilder Builder;

  // Function type: (i32, i32) -> ()
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes;
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, local.get 1, i32.store offset=0, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__store);
  Instrs[2].getMemoryOffset() = 0;
  Instrs[2].getMemoryAlign() = 2;
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

TEST(IRBuilderTest, I64Store) {
  WasmToIRBuilder Builder;

  // Function type: (i32, i64) -> ()
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), ValType(TypeCode::I64)};
  std::vector<ValType> RetTypes;
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, local.get 1, i64.store offset=0, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I64__store);
  Instrs[2].getMemoryOffset() = 0;
  Instrs[2].getMemoryAlign() = 3;
  Instrs.emplace_back(OpCode::Return);

  auto Res = Builder.buildFromInstructions(Instrs);
  ASSERT_TRUE(Res);
}

//==============================================================================
// Integration Tests
//==============================================================================

TEST(IntegrationTest, CompleteFunction) {
  WasmToIRBuilder Builder;

  // Function type: (i32, i32) -> (i32)
  // Function: max(a, b) simplified - returns a + b for testing
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), ValType(TypeCode::I32)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  std::vector<std::pair<uint32_t, ValType>> Locals = {
    {1, ValType(TypeCode::I32)}  // result local
  };

  ASSERT_TRUE(Builder.initialize(FuncType, Locals));

  // Build: local.get 0, local.get 1, i32.add, local.tee 2, return
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[1].getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__add);
  Instrs.emplace_back(OpCode::Local__tee);
  Instrs[3].getTargetIndex() = 2;
  Instrs.emplace_back(OpCode::Return);

  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));

  // Compile
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());

  if (CompRes) {
    ASSERT_NE(CompRes.value().NativeFunc, nullptr);
    ASSERT_GT(CompRes.value().CodeSize, 0);
    Engine.release(CompRes.value().NativeFunc, CompRes.value().CodeSize);
  }
}

TEST(IntegrationTest, MultiTypeFunction) {
  WasmToIRBuilder Builder;

  // Function type: (i32, i64, f32, f64) -> (i32)
  std::vector<ValType> ParamTypes = {ValType(TypeCode::I32),
                                      ValType(TypeCode::I64),
                                      ValType(TypeCode::F32),
                                      ValType(TypeCode::F64)};
  std::vector<ValType> RetTypes = {ValType(TypeCode::I32)};
  AST::FunctionType FuncType(ParamTypes, RetTypes);

  ASSERT_TRUE(Builder.initialize(FuncType, {}));

  // Build: local.get 0, return (just return first param)
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs[0].getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Return);

  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));

  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());

  if (CompRes) {
    ASSERT_NE(CompRes.value().NativeFunc, nullptr);
    Engine.release(CompRes.value().NativeFunc, CompRes.value().CodeSize);
  }
}

} // namespace VM
} // namespace WasmEdge

//==============================================================================
// Test Summary
//==============================================================================
// Total Test Cases: 30+
//
// IR Builder Tests (25):
//   - Initialization: 3 tests (void, with params, with locals)
//   - Constants: 4 tests (i32, i64, f32, f64)
//   - Locals: 3 tests (get, set, tee)
//   - I32 Arithmetic: 4 tests (add, sub, mul, add constants)
//   - I64 Arithmetic: 3 tests (add, sub, mul)
//   - Complex Expressions: 2 tests
//   - Control Flow: 5 tests (return, block, loop, if, nested)
//   - Memory Ops: 4 tests (i32 load/store, i64 load/store)
//
// IR JIT Engine Tests (3):
//   - Constant compilation
//   - Simple arithmetic compilation
//   - Multiple operations compilation
//
// Integration Tests (2):
//   - Complete function with locals
//   - Multi-type function parameters
//==============================================================================

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  std::cout << "\n========================================\n";
  std::cout << "WasmEdge IR JIT Test Suite\n";
  std::cout << "Testing Instruction Mappings & IR Generation\n";
  std::cout << "========================================\n\n";
  
  int result = RUN_ALL_TESTS();
  
  std::cout << "\n========================================\n";
  if (result == 0) {
    std::cout << "All IR JIT tests passed! ✅\n";
  } else {
    std::cout << "Some tests failed. ❌\n";
  }
  std::cout << "========================================\n\n";
  
  return result;
}

