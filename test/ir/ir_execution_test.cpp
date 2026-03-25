// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//==============================================================================
// IR JIT Execution Correctness Test Suite
//
// This test suite validates that JIT-compiled code produces CORRECT RESULTS
// by actually executing the generated native code and comparing outputs.
//
// Unlike ir_basic_test.cpp and ir_instruction_test.cpp which only verify
// IR generation, these tests verify computational correctness.
//
// Test Coverage:
// - I32 arithmetic: add, sub, mul, div_s, div_u, rem_s, rem_u
// - I64 arithmetic: add, sub, mul, div_s, div_u, rem_s, rem_u
// - Comparisons: eq, ne, lt_s, lt_u, le_s, le_u, gt_s, gt_u, ge_s, ge_u
// - Unary operations: eqz, clz, ctz, popcnt
// - Control flow: if/else, block/br, br_if, loop, nested blocks, early return
// - Edge cases: overflow, underflow, division by zero behavior
//
// Build with: -DWASMEDGE_BUILD_IR_JIT=ON
// Run with: ./wasmedgeIRExecutionTests
//==============================================================================

#include "vm/ir_builder.h"
#include "vm/ir_jit_engine.h"

#include "ast/instruction.h"
#include "ast/type.h"
#include "common/types.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <limits>
#include <vector>
#include <cmath>

namespace WasmEdge {
namespace VM {

//==============================================================================
// Test Fixture with Helper Methods
//==============================================================================

class IRExecutionTest : public ::testing::Test {
protected:
  // Dummy memory buffer for JIT calls (memory base is required parameter)
  std::vector<uint8_t> Memory = std::vector<uint8_t>(65536, 0);

  // Build a binary operation function: (a, b) -> result
  // Returns compiled function pointer or nullptr on failure
  void* buildBinaryOp(OpCode Op, TypeCode Type1, TypeCode Type2, TypeCode RetType) {
    Builder.reset();
    
    std::vector<ValType> ParamTypes = {ValType(Type1), ValType(Type2)};
    std::vector<ValType> RetTypes = {ValType(RetType)};
    AST::FunctionType FuncType(ParamTypes, RetTypes);
    
    auto InitRes = Builder.initialize(FuncType, {});
    if (!InitRes) return nullptr;
    
    // Build: local.get 0, local.get 1, <op>, return
    std::vector<AST::Instruction> Instrs;
    Instrs.emplace_back(OpCode::Local__get);
    Instrs[0].getTargetIndex() = 0;
    Instrs.emplace_back(OpCode::Local__get);
    Instrs[1].getTargetIndex() = 1;
    Instrs.emplace_back(Op);
    Instrs.emplace_back(OpCode::Return);
    
    auto BuildRes = Builder.buildFromInstructions(Instrs);
    if (!BuildRes) return nullptr;
    
    auto CompRes = Engine.compile(Builder.getIRContext());
    if (!CompRes) return nullptr;
    
    return CompRes->NativeFunc;
  }

  // Build a unary operation function: (a) -> result
  void* buildUnaryOp(OpCode Op, TypeCode InType, TypeCode RetType) {
    Builder.reset();
    
    std::vector<ValType> ParamTypes = {ValType(InType)};
    std::vector<ValType> RetTypes = {ValType(RetType)};
    AST::FunctionType FuncType(ParamTypes, RetTypes);
    
    auto InitRes = Builder.initialize(FuncType, {});
    if (!InitRes) return nullptr;
    
    // Build: local.get 0, <op>, return
    std::vector<AST::Instruction> Instrs;
    Instrs.emplace_back(OpCode::Local__get);
    Instrs[0].getTargetIndex() = 0;
    Instrs.emplace_back(Op);
    Instrs.emplace_back(OpCode::Return);
    
    auto BuildRes = Builder.buildFromInstructions(Instrs);
    if (!BuildRes) return nullptr;
    
    auto CompRes = Engine.compile(Builder.getIRContext());
    if (!CompRes) return nullptr;
    
    return CompRes->NativeFunc;
  }

  // Execute via uniform JIT convention: ret func(JitExecEnv* env, uint64_t* args)
  int32_t execI32Binary(void* Func, int32_t a, int32_t b) {
    AST::FunctionType FuncType(
        {ValType(TypeCode::I32), ValType(TypeCode::I32)},
        {ValType(TypeCode::I32)});
    std::vector<ValVariant> Args = {ValVariant(static_cast<uint32_t>(a)),
                                    ValVariant(static_cast<uint32_t>(b))};
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return static_cast<int32_t>(Rets[0].get<uint32_t>());
  }

  uint32_t execU32Binary(void* Func, uint32_t a, uint32_t b) {
    AST::FunctionType FuncType(
        {ValType(TypeCode::I32), ValType(TypeCode::I32)},
        {ValType(TypeCode::I32)});
    std::vector<ValVariant> Args = {ValVariant(a), ValVariant(b)};
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return Rets[0].get<uint32_t>();
  }

  int64_t execI64Binary(void* Func, int64_t a, int64_t b) {
    AST::FunctionType FuncType(
        {ValType(TypeCode::I64), ValType(TypeCode::I64)},
        {ValType(TypeCode::I64)});
    std::vector<ValVariant> Args = {ValVariant(static_cast<uint64_t>(a)),
                                    ValVariant(static_cast<uint64_t>(b))};
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return static_cast<int64_t>(Rets[0].get<uint64_t>());
  }

  uint64_t execU64Binary(void* Func, uint64_t a, uint64_t b) {
    AST::FunctionType FuncType(
        {ValType(TypeCode::I64), ValType(TypeCode::I64)},
        {ValType(TypeCode::I64)});
    std::vector<ValVariant> Args = {ValVariant(a), ValVariant(b)};
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return Rets[0].get<uint64_t>();
  }

  int32_t execI32Unary(void* Func, int32_t a) {
    AST::FunctionType FuncType({ValType(TypeCode::I32)},
                               {ValType(TypeCode::I32)});
    std::vector<ValVariant> Args = {ValVariant(static_cast<uint32_t>(a))};
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return static_cast<int32_t>(Rets[0].get<uint32_t>());
  }

  int64_t execI64Unary(void* Func, int64_t a) {
    AST::FunctionType FuncType({ValType(TypeCode::I64)},
                               {ValType(TypeCode::I64)});
    std::vector<ValVariant> Args = {ValVariant(static_cast<uint64_t>(a))};
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return static_cast<int64_t>(Rets[0].get<uint64_t>());
  }

  int32_t execI32Compare(void* Func, int32_t a, int32_t b) {
    AST::FunctionType FuncType(
        {ValType(TypeCode::I32), ValType(TypeCode::I32)},
        {ValType(TypeCode::I32)});
    std::vector<ValVariant> Args = {ValVariant(static_cast<uint32_t>(a)),
                                    ValVariant(static_cast<uint32_t>(b))};
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return static_cast<int32_t>(Rets[0].get<uint32_t>());
  }

  int32_t execI64Compare(void* Func, int64_t a, int64_t b) {
    AST::FunctionType FuncType(
        {ValType(TypeCode::I64), ValType(TypeCode::I64)},
        {ValType(TypeCode::I32)});
    std::vector<ValVariant> Args = {ValVariant(static_cast<uint64_t>(a)),
                                    ValVariant(static_cast<uint64_t>(b))};
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return static_cast<int32_t>(Rets[0].get<uint32_t>());
  }

  int32_t execI32NoParams(void* Func) {
    AST::FunctionType FuncType({}, {ValType(TypeCode::I32)});
    std::vector<ValVariant> Args;
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return static_cast<int32_t>(Rets[0].get<uint32_t>());
  }

  int32_t execI32OneParam(void* Func, int32_t a) {
    AST::FunctionType FuncType({ValType(TypeCode::I32)},
                               {ValType(TypeCode::I32)});
    std::vector<ValVariant> Args = {ValVariant(static_cast<uint32_t>(a))};
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return static_cast<int32_t>(Rets[0].get<uint32_t>());
  }

  // (i64) -> i32 (e.g. i64.eqz)
  int32_t execI64UnaryReturnI32(void* Func, int64_t a) {
    AST::FunctionType FuncType({ValType(TypeCode::I64)},
                               {ValType(TypeCode::I32)});
    std::vector<ValVariant> Args = {ValVariant(static_cast<uint64_t>(a))};
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return static_cast<int32_t>(Rets[0].get<uint32_t>());
  }

  // Generic invoke: (Func, FuncType, Args) -> single i32 return
  int32_t invokeI32(void* Func, const AST::FunctionType& FuncType,
                    std::vector<ValVariant> Args) {
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return static_cast<int32_t>(Rets[0].get<uint32_t>());
  }

  int64_t invokeI64(void* Func, const AST::FunctionType& FuncType,
                    std::vector<ValVariant> Args) {
    std::vector<ValVariant> Rets(1);
    if (!Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                       Memory.data(), Memory.size()))
      return 0;
    return static_cast<int64_t>(Rets[0].get<uint64_t>());
  }

  void invokeVoid(void* Func, const AST::FunctionType& FuncType,
                  std::vector<ValVariant> Args) {
    std::vector<ValVariant> Rets;
    Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0, nullptr,
                  Memory.data(), Memory.size());
  }

  // Build a custom function with given instructions
  // Instructions are built by the provided lambda
  void* buildCustomFunc(
      const std::vector<ValType>& ParamTypes,
      const std::vector<ValType>& RetTypes,
      const std::vector<std::pair<uint32_t, ValType>>& Locals,
      const std::vector<AST::Instruction>& Instrs) {
    Builder.reset();
    
    AST::FunctionType FuncType(ParamTypes, RetTypes);
    
    auto InitRes = Builder.initialize(FuncType, Locals);
    if (!InitRes) return nullptr;
    
    auto BuildRes = Builder.buildFromInstructions(Instrs);
    if (!BuildRes) return nullptr;
    
    auto CompRes = Engine.compile(Builder.getIRContext());
    if (!CompRes) return nullptr;
    
    return CompRes->NativeFunc;
  }

  WasmToIRBuilder Builder;
  IRJitEngine Engine;
};

//==============================================================================
// I32 Arithmetic Execution Tests
//==============================================================================

TEST_F(IRExecutionTest, I32_Add_Basic) {
  void* Func = buildBinaryOp(OpCode::I32__add, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr) << "Failed to compile i32.add";

  // Basic cases
  EXPECT_EQ(execI32Binary(Func, 10, 20), 30);
  EXPECT_EQ(execI32Binary(Func, 0, 0), 0);
  EXPECT_EQ(execI32Binary(Func, -5, 5), 0);
  EXPECT_EQ(execI32Binary(Func, -10, -20), -30);
  EXPECT_EQ(execI32Binary(Func, 100, -50), 50);
}

TEST_F(IRExecutionTest, I32_Add_Overflow) {
  void* Func = buildBinaryOp(OpCode::I32__add, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  // Overflow wraps around (WebAssembly behavior)
  EXPECT_EQ(execI32Binary(Func, INT32_MAX, 1), INT32_MIN);
  EXPECT_EQ(execI32Binary(Func, INT32_MIN, -1), INT32_MAX);
}

TEST_F(IRExecutionTest, I32_Sub_Basic) {
  void* Func = buildBinaryOp(OpCode::I32__sub, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr) << "Failed to compile i32.sub";

  EXPECT_EQ(execI32Binary(Func, 30, 10), 20);
  EXPECT_EQ(execI32Binary(Func, 0, 0), 0);
  EXPECT_EQ(execI32Binary(Func, 5, 10), -5);
  EXPECT_EQ(execI32Binary(Func, -10, -20), 10);
  EXPECT_EQ(execI32Binary(Func, 0, 1), -1);
}

TEST_F(IRExecutionTest, I32_Sub_Overflow) {
  void* Func = buildBinaryOp(OpCode::I32__sub, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Binary(Func, INT32_MIN, 1), INT32_MAX);
  EXPECT_EQ(execI32Binary(Func, INT32_MAX, -1), INT32_MIN);
}

TEST_F(IRExecutionTest, I32_Mul_Basic) {
  void* Func = buildBinaryOp(OpCode::I32__mul, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr) << "Failed to compile i32.mul";

  EXPECT_EQ(execI32Binary(Func, 6, 7), 42);
  EXPECT_EQ(execI32Binary(Func, 0, 100), 0);
  EXPECT_EQ(execI32Binary(Func, 1, -1), -1);
  EXPECT_EQ(execI32Binary(Func, -3, -4), 12);
  EXPECT_EQ(execI32Binary(Func, -3, 4), -12);
}

TEST_F(IRExecutionTest, I32_Mul_Overflow) {
  void* Func = buildBinaryOp(OpCode::I32__mul, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  // Overflow wraps
  EXPECT_EQ(execI32Binary(Func, INT32_MAX, 2), -2);  // 0x7FFFFFFF * 2 = 0xFFFFFFFE = -2
}

TEST_F(IRExecutionTest, I32_Div_S_Basic) {
  void* Func = buildBinaryOp(OpCode::I32__div_s, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr) << "Failed to compile i32.div_s";

  EXPECT_EQ(execI32Binary(Func, 20, 4), 5);
  EXPECT_EQ(execI32Binary(Func, 20, 3), 6);  // Truncates toward zero
  EXPECT_EQ(execI32Binary(Func, -20, 4), -5);
  EXPECT_EQ(execI32Binary(Func, 20, -4), -5);
  EXPECT_EQ(execI32Binary(Func, -20, -4), 5);
  EXPECT_EQ(execI32Binary(Func, 7, 3), 2);
  EXPECT_EQ(execI32Binary(Func, -7, 3), -2);  // Truncates toward zero, not floor
}

TEST_F(IRExecutionTest, I32_Div_U_Basic) {
  void* Func = buildBinaryOp(OpCode::I32__div_u, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr) << "Failed to compile i32.div_u";

  EXPECT_EQ(execU32Binary(Func, 20, 4), 5u);
  EXPECT_EQ(execU32Binary(Func, 20, 3), 6u);
  // -1 as unsigned is UINT32_MAX
  EXPECT_EQ(execU32Binary(Func, 0xFFFFFFFF, 2), 0x7FFFFFFFu);
}

TEST_F(IRExecutionTest, I32_Rem_S_Basic) {
  void* Func = buildBinaryOp(OpCode::I32__rem_s, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr) << "Failed to compile i32.rem_s";

  EXPECT_EQ(execI32Binary(Func, 7, 3), 1);
  EXPECT_EQ(execI32Binary(Func, -7, 3), -1);   // Sign follows dividend
  EXPECT_EQ(execI32Binary(Func, 7, -3), 1);
  EXPECT_EQ(execI32Binary(Func, -7, -3), -1);
  EXPECT_EQ(execI32Binary(Func, 8, 4), 0);
}

TEST_F(IRExecutionTest, I32_Rem_U_Basic) {
  void* Func = buildBinaryOp(OpCode::I32__rem_u, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr) << "Failed to compile i32.rem_u";

  EXPECT_EQ(execU32Binary(Func, 7, 3), 1u);
  EXPECT_EQ(execU32Binary(Func, 8, 4), 0u);
  EXPECT_EQ(execU32Binary(Func, 0xFFFFFFFF, 10), 5u);  // UINT32_MAX % 10 = 5
}

//==============================================================================
// I32 Bitwise Operation Execution Tests
//==============================================================================

TEST_F(IRExecutionTest, I32_And) {
  void* Func = buildBinaryOp(OpCode::I32__and, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Binary(Func, 0xFF, 0x0F), 0x0F);
  EXPECT_EQ(execI32Binary(Func, 0xAAAAAAAA, 0x55555555), 0);
  EXPECT_EQ(execI32Binary(Func, -1, 0xFF), 0xFF);
}

TEST_F(IRExecutionTest, I32_Or) {
  void* Func = buildBinaryOp(OpCode::I32__or, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Binary(Func, 0xF0, 0x0F), 0xFF);
  EXPECT_EQ(execI32Binary(Func, 0, 0), 0);
  EXPECT_EQ(execI32Binary(Func, 0xAAAAAAAA, 0x55555555), -1);  // All bits set
}

TEST_F(IRExecutionTest, I32_Xor) {
  void* Func = buildBinaryOp(OpCode::I32__xor, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Binary(Func, 0xFF, 0xFF), 0);
  EXPECT_EQ(execI32Binary(Func, 0xFF, 0x00), 0xFF);
  EXPECT_EQ(execI32Binary(Func, 0xAAAAAAAA, 0x55555555), -1);
}

TEST_F(IRExecutionTest, I32_Shl) {
  void* Func = buildBinaryOp(OpCode::I32__shl, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Binary(Func, 1, 4), 16);
  EXPECT_EQ(execI32Binary(Func, 1, 31), INT32_MIN);
  EXPECT_EQ(execI32Binary(Func, 0xFF, 8), 0xFF00);
  // Shift amount is masked to 5 bits (mod 32)
  EXPECT_EQ(execI32Binary(Func, 1, 32), 1);  // 32 mod 32 = 0
}

TEST_F(IRExecutionTest, I32_Shr_S) {
  void* Func = buildBinaryOp(OpCode::I32__shr_s, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Binary(Func, 16, 2), 4);
  EXPECT_EQ(execI32Binary(Func, -16, 2), -4);  // Sign-extends
  EXPECT_EQ(execI32Binary(Func, INT32_MIN, 31), -1);  // All 1s from sign extension
}

TEST_F(IRExecutionTest, I32_Shr_U) {
  void* Func = buildBinaryOp(OpCode::I32__shr_u, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execU32Binary(Func, 16, 2), 4u);
  EXPECT_EQ(execU32Binary(Func, 0x80000000, 31), 1u);  // Zero-extends
  EXPECT_EQ(execU32Binary(Func, 0xFFFFFFFF, 4), 0x0FFFFFFFu);
}

TEST_F(IRExecutionTest, I32_Rotl) {
  void* Func = buildBinaryOp(OpCode::I32__rotl, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execU32Binary(Func, 0x80000001, 1), 0x00000003u);
  EXPECT_EQ(execU32Binary(Func, 0x12345678, 8), 0x34567812u);
}

TEST_F(IRExecutionTest, I32_Rotr) {
  void* Func = buildBinaryOp(OpCode::I32__rotr, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execU32Binary(Func, 0x80000001, 1), 0xC0000000u);
  EXPECT_EQ(execU32Binary(Func, 0x12345678, 8), 0x78123456u);
}

//==============================================================================
// I32 Comparison Execution Tests
//==============================================================================

TEST_F(IRExecutionTest, I32_Eq) {
  void* Func = buildBinaryOp(OpCode::I32__eq, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Compare(Func, 5, 5), 1);
  EXPECT_EQ(execI32Compare(Func, 5, 6), 0);
  EXPECT_EQ(execI32Compare(Func, -1, -1), 1);
  EXPECT_EQ(execI32Compare(Func, 0, 0), 1);
}

TEST_F(IRExecutionTest, I32_Ne) {
  void* Func = buildBinaryOp(OpCode::I32__ne, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Compare(Func, 5, 5), 0);
  EXPECT_EQ(execI32Compare(Func, 5, 6), 1);
  EXPECT_EQ(execI32Compare(Func, -1, 1), 1);
}

TEST_F(IRExecutionTest, I32_Lt_S) {
  void* Func = buildBinaryOp(OpCode::I32__lt_s, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Compare(Func, 5, 10), 1);
  EXPECT_EQ(execI32Compare(Func, 10, 5), 0);
  EXPECT_EQ(execI32Compare(Func, 5, 5), 0);
  EXPECT_EQ(execI32Compare(Func, -5, 5), 1);   // -5 < 5 signed
  EXPECT_EQ(execI32Compare(Func, -1, -2), 0);  // -1 > -2 signed
}

TEST_F(IRExecutionTest, I32_Lt_U) {
  void* Func = buildBinaryOp(OpCode::I32__lt_u, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Compare(Func, 5, 10), 1);
  EXPECT_EQ(execI32Compare(Func, 10, 5), 0);
  // -1 as unsigned is MAX, so 5 < MAX
  EXPECT_EQ(execI32Compare(Func, 5, -1), 1);
  EXPECT_EQ(execI32Compare(Func, -1, 5), 0);
}

TEST_F(IRExecutionTest, I32_Le_S) {
  void* Func = buildBinaryOp(OpCode::I32__le_s, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Compare(Func, 5, 10), 1);
  EXPECT_EQ(execI32Compare(Func, 5, 5), 1);
  EXPECT_EQ(execI32Compare(Func, 10, 5), 0);
}

TEST_F(IRExecutionTest, I32_Gt_S) {
  void* Func = buildBinaryOp(OpCode::I32__gt_s, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Compare(Func, 10, 5), 1);
  EXPECT_EQ(execI32Compare(Func, 5, 10), 0);
  EXPECT_EQ(execI32Compare(Func, 5, 5), 0);
  EXPECT_EQ(execI32Compare(Func, 5, -5), 1);
}

TEST_F(IRExecutionTest, I32_Ge_S) {
  void* Func = buildBinaryOp(OpCode::I32__ge_s, TypeCode::I32, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Compare(Func, 10, 5), 1);
  EXPECT_EQ(execI32Compare(Func, 5, 5), 1);
  EXPECT_EQ(execI32Compare(Func, 5, 10), 0);
}

//==============================================================================
// I32 Unary Operation Execution Tests
//==============================================================================

TEST_F(IRExecutionTest, I32_Eqz) {
  void* Func = buildUnaryOp(OpCode::I32__eqz, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Unary(Func, 0), 1);
  EXPECT_EQ(execI32Unary(Func, 1), 0);
  EXPECT_EQ(execI32Unary(Func, -1), 0);
  EXPECT_EQ(execI32Unary(Func, 100), 0);
}

TEST_F(IRExecutionTest, I32_Clz) {
  void* Func = buildUnaryOp(OpCode::I32__clz, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Unary(Func, 0), 32);
  EXPECT_EQ(execI32Unary(Func, 1), 31);
  EXPECT_EQ(execI32Unary(Func, 0x80000000), 0);
  EXPECT_EQ(execI32Unary(Func, 0x00008000), 16);
  EXPECT_EQ(execI32Unary(Func, -1), 0);
}

TEST_F(IRExecutionTest, I32_Ctz) {
  void* Func = buildUnaryOp(OpCode::I32__ctz, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Unary(Func, 0), 32);
  EXPECT_EQ(execI32Unary(Func, 1), 0);
  EXPECT_EQ(execI32Unary(Func, 2), 1);
  EXPECT_EQ(execI32Unary(Func, 0x80000000), 31);
  EXPECT_EQ(execI32Unary(Func, 0x00010000), 16);
}

TEST_F(IRExecutionTest, I32_Popcnt) {
  void* Func = buildUnaryOp(OpCode::I32__popcnt, TypeCode::I32, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI32Unary(Func, 0), 0);
  EXPECT_EQ(execI32Unary(Func, 1), 1);
  EXPECT_EQ(execI32Unary(Func, -1), 32);  // All bits set
  EXPECT_EQ(execI32Unary(Func, 0x55555555), 16);
  EXPECT_EQ(execI32Unary(Func, 0x0F0F0F0F), 16);
}

//==============================================================================
// I64 Arithmetic Execution Tests
//==============================================================================

TEST_F(IRExecutionTest, I64_Add_Basic) {
  void* Func = buildBinaryOp(OpCode::I64__add, TypeCode::I64, TypeCode::I64, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Binary(Func, 10, 20), 30);
  EXPECT_EQ(execI64Binary(Func, -5, 5), 0);
  EXPECT_EQ(execI64Binary(Func, 0x100000000LL, 0x100000000LL), 0x200000000LL);
}

TEST_F(IRExecutionTest, I64_Add_Overflow) {
  void* Func = buildBinaryOp(OpCode::I64__add, TypeCode::I64, TypeCode::I64, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Binary(Func, INT64_MAX, 1), INT64_MIN);
}

TEST_F(IRExecutionTest, I64_Sub_Basic) {
  void* Func = buildBinaryOp(OpCode::I64__sub, TypeCode::I64, TypeCode::I64, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Binary(Func, 30, 10), 20);
  EXPECT_EQ(execI64Binary(Func, 0, 1), -1);
  EXPECT_EQ(execI64Binary(Func, 0x200000000LL, 0x100000000LL), 0x100000000LL);
}

TEST_F(IRExecutionTest, I64_Mul_Basic) {
  void* Func = buildBinaryOp(OpCode::I64__mul, TypeCode::I64, TypeCode::I64, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Binary(Func, 6, 7), 42);
  EXPECT_EQ(execI64Binary(Func, -3, 4), -12);
  EXPECT_EQ(execI64Binary(Func, 0x10000, 0x10000), 0x100000000LL);
}

TEST_F(IRExecutionTest, I64_Div_S_Basic) {
  void* Func = buildBinaryOp(OpCode::I64__div_s, TypeCode::I64, TypeCode::I64, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Binary(Func, 20, 4), 5);
  EXPECT_EQ(execI64Binary(Func, -20, 4), -5);
  EXPECT_EQ(execI64Binary(Func, 0x100000000LL, 2), 0x80000000LL);
}

TEST_F(IRExecutionTest, I64_Div_U_Basic) {
  void* Func = buildBinaryOp(OpCode::I64__div_u, TypeCode::I64, TypeCode::I64, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execU64Binary(Func, 20, 4), 5u);
  EXPECT_EQ(execU64Binary(Func, 0xFFFFFFFFFFFFFFFFULL, 2), 0x7FFFFFFFFFFFFFFFULL);
}

TEST_F(IRExecutionTest, I64_Rem_S_Basic) {
  void* Func = buildBinaryOp(OpCode::I64__rem_s, TypeCode::I64, TypeCode::I64, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Binary(Func, 7, 3), 1);
  EXPECT_EQ(execI64Binary(Func, -7, 3), -1);
  EXPECT_EQ(execI64Binary(Func, 7, -3), 1);
}

//==============================================================================
// I64 Comparison Execution Tests
//==============================================================================

TEST_F(IRExecutionTest, I64_Eq) {
  void* Func = buildBinaryOp(OpCode::I64__eq, TypeCode::I64, TypeCode::I64, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Compare(Func, 5, 5), 1);
  EXPECT_EQ(execI64Compare(Func, 0x100000000LL, 0x100000000LL), 1);
  EXPECT_EQ(execI64Compare(Func, 5, 6), 0);
}

TEST_F(IRExecutionTest, I64_Lt_S) {
  void* Func = buildBinaryOp(OpCode::I64__lt_s, TypeCode::I64, TypeCode::I64, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Compare(Func, 5, 10), 1);
  EXPECT_EQ(execI64Compare(Func, -1, 1), 1);
  EXPECT_EQ(execI64Compare(Func, 10, 5), 0);
}

TEST_F(IRExecutionTest, I64_Lt_U) {
  void* Func = buildBinaryOp(OpCode::I64__lt_u, TypeCode::I64, TypeCode::I64, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Compare(Func, 5, 10), 1);
  // -1 as unsigned is MAX
  EXPECT_EQ(execI64Compare(Func, 5, -1), 1);
  EXPECT_EQ(execI64Compare(Func, -1, 5), 0);
}

//==============================================================================
// I64 Unary Operation Execution Tests
//==============================================================================

TEST_F(IRExecutionTest, I64_Eqz) {
  void* Func = buildUnaryOp(OpCode::I64__eqz, TypeCode::I64, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64UnaryReturnI32(Func, 0), 1);
  EXPECT_EQ(execI64UnaryReturnI32(Func, 1), 0);
  EXPECT_EQ(execI64UnaryReturnI32(Func, 0x100000000LL), 0);
}

TEST_F(IRExecutionTest, I64_Clz) {
  void* Func = buildUnaryOp(OpCode::I64__clz, TypeCode::I64, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Unary(Func, 0), 64);
  EXPECT_EQ(execI64Unary(Func, 1), 63);
  EXPECT_EQ(execI64Unary(Func, 0x8000000000000000LL), 0);
  EXPECT_EQ(execI64Unary(Func, 0x0000000100000000LL), 31);
}

TEST_F(IRExecutionTest, I64_Ctz) {
  void* Func = buildUnaryOp(OpCode::I64__ctz, TypeCode::I64, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Unary(Func, 0), 64);
  EXPECT_EQ(execI64Unary(Func, 1), 0);
  EXPECT_EQ(execI64Unary(Func, 0x8000000000000000LL), 63);
  EXPECT_EQ(execI64Unary(Func, 0x0000000100000000LL), 32);
}

TEST_F(IRExecutionTest, I64_Popcnt) {
  void* Func = buildUnaryOp(OpCode::I64__popcnt, TypeCode::I64, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  EXPECT_EQ(execI64Unary(Func, 0), 0);
  EXPECT_EQ(execI64Unary(Func, -1), 64);
  EXPECT_EQ(execI64Unary(Func, 0x5555555555555555LL), 32);
}

//==============================================================================
// Control Flow Execution Tests
//
// These tests verify that control flow constructs not only compile correctly
// but also execute with correct results.
//
// Test coverage:
// - If/else with return in both branches
// - If/else with different return values
// - Nested if/else
// - Early return from function
// - Multiple conditions (chained if/else)
//==============================================================================

// Test: if (cond) return 1 else return 0
TEST_F(IRExecutionTest, ControlFlow_IfElse_Basic) {
  std::vector<AST::Instruction> Instrs;
  
  // if (param) return 1 else return 0
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::If);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(1));
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::Else);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::End);
  // Dead code after if/else (both branches return)
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(99));
  Instrs.emplace_back(OpCode::End);
  
  void* Func = buildCustomFunc(
    {ValType(TypeCode::I32)},
    {ValType(TypeCode::I32)},
    {},
    Instrs
  );
  
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)},
                             {ValType(TypeCode::I32)});
  // Test with true condition (non-zero)
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(1u)}), 1);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(42u)}), 1);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(static_cast<uint32_t>(-1))}), 1);
  // Test with false condition (zero)
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 0);
}

// Test: if (a > b) return a else return b (max function)
TEST_F(IRExecutionTest, ControlFlow_IfElse_Max) {
  std::vector<AST::Instruction> Instrs;
  
  // if (a > b) return a else return b
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;  // a
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;  // b
  Instrs.emplace_back(OpCode::I32__gt_s);  // a > b
  Instrs.emplace_back(OpCode::If);
  // Then branch: return a
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::Else);
  // Else branch: return b
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::End);
  // Dead code
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::End);
  
  void* Func = buildCustomFunc(
    {ValType(TypeCode::I32), ValType(TypeCode::I32)},
    {ValType(TypeCode::I32)},
    {},
    Instrs
  );
  
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType(
      {ValType(TypeCode::I32), ValType(TypeCode::I32)},
      {ValType(TypeCode::I32)});
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(10u), ValVariant(5u)}), 10);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(5u), ValVariant(10u)}), 10);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(7u), ValVariant(7u)}), 7);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(static_cast<uint32_t>(-5)), ValVariant(static_cast<uint32_t>(-10))}), -5);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(static_cast<uint32_t>(-10)), ValVariant(5u)}), 5);
}

// Test: Nested if/else - sign function: returns -1, 0, or 1
TEST_F(IRExecutionTest, ControlFlow_NestedIfElse_Sign) {
  std::vector<AST::Instruction> Instrs;
  
  // if (x < 0) return -1
  // else if (x > 0) return 1
  // else return 0
  
  // First check: x < 0
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::I32__lt_s);
  Instrs.emplace_back(OpCode::If);
  // x < 0: return -1
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(-1));
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::Else);
  // Nested: check x > 0
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::I32__gt_s);
  Instrs.emplace_back(OpCode::If);
  // x > 0: return 1
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(1));
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::Else);
  // x == 0: return 0
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::End);  // inner if
  Instrs.emplace_back(OpCode::End);  // outer if
  // Dead code
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(99));
  Instrs.emplace_back(OpCode::End);
  
  void* Func = buildCustomFunc(
    {ValType(TypeCode::I32)},
    {ValType(TypeCode::I32)},
    {},
    Instrs
  );
  
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)},
                             {ValType(TypeCode::I32)});
  // sign(x) tests
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(static_cast<uint32_t>(-100))}), -1);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(static_cast<uint32_t>(-1))}), -1);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 0);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(1u)}), 1);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(100u)}), 1);
}

// Test: Early return - clamp function
TEST_F(IRExecutionTest, ControlFlow_EarlyReturn_Clamp) {
  std::vector<AST::Instruction> Instrs;
  
  // clamp(x, min=0, max=100)
  // if (x < 0) return 0
  // if (x > 100) return 100
  // return x
  
  // Check x < 0
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::I32__lt_s);
  Instrs.emplace_back(OpCode::If);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::End);  // if x < 0
  
  // Check x > 100
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(100));
  Instrs.emplace_back(OpCode::I32__gt_s);
  Instrs.emplace_back(OpCode::If);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(100));
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::End);  // if x > 100
  
  // Default: return x
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::End);  // function
  
  void* Func = buildCustomFunc(
    {ValType(TypeCode::I32)},
    {ValType(TypeCode::I32)},
    {},
    Instrs
  );
  
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)},
                             {ValType(TypeCode::I32)});
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(static_cast<uint32_t>(-50))}), 0);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(static_cast<uint32_t>(-1))}), 0);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 0);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(1u)}), 1);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(50u)}), 50);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(99u)}), 99);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(100u)}), 100);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(101u)}), 100);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(200u)}), 100);
}

// Test: Absolute value using if/else
TEST_F(IRExecutionTest, ControlFlow_IfElse_Abs) {
  std::vector<AST::Instruction> Instrs;
  
  // abs(x): if (x < 0) return -x else return x
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::I32__lt_s);
  Instrs.emplace_back(OpCode::If);
  // x < 0: return 0 - x
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::I32__sub);
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::Else);
  // x >= 0: return x
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::End);
  // Dead code
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::End);
  
  void* Func = buildCustomFunc(
    {ValType(TypeCode::I32)},
    {ValType(TypeCode::I32)},
    {},
    Instrs
  );
  
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)},
                             {ValType(TypeCode::I32)});
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 0);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(1u)}), 1);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(static_cast<uint32_t>(-1))}), 1);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(100u)}), 100);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(static_cast<uint32_t>(-100))}), 100);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(static_cast<uint32_t>(INT32_MAX))}), INT32_MAX);
}

// Test: br_table - switch statement (minimal: default only)
TEST_F(IRExecutionTest, ControlFlow_BrTable_DefaultOnly) {
  std::vector<AST::Instruction> Instrs;
  
  // Minimal br_table: just default case
  // block $default
  //   local.get 0
  //   br_table $default   (only default, no cases)
  // end
  // i32.const 42
  // return
  
  Instrs.emplace_back(OpCode::Block);
  
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  
  // br_table with only default (no indexed cases)
  Instrs.emplace_back(OpCode::Br_table);
  Instrs.back().setLabelListSize(1);  // just default
  auto LabelList = Instrs.back().getLabelList();
  LabelList[0].TargetIndex = 0;  // default -> $default
  
  Instrs.emplace_back(OpCode::End);  // end of block
  
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(42));
  Instrs.emplace_back(OpCode::Return);
  
  Instrs.emplace_back(OpCode::End);  // function end
  
  void* Func = buildCustomFunc(
    {ValType(TypeCode::I32)},
    {ValType(TypeCode::I32)},
    {},
    Instrs
  );
  
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)},
                             {ValType(TypeCode::I32)});
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 42);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(1u)}), 42);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(100u)}), 42);
}

// Test: br_table - switch statement
TEST_F(IRExecutionTest, ControlFlow_BrTable_Switch) {
  std::vector<AST::Instruction> Instrs;
  
  // Implements a simple switch:
  // switch(x) {
  //   case 0: return 100;
  //   case 1: return 200;
  //   case 2: return 300;
  //   default: return 0;
  // }
  
  // block $default
  Instrs.emplace_back(OpCode::Block);
  //   block $case2
  Instrs.emplace_back(OpCode::Block);
  //     block $case1
  Instrs.emplace_back(OpCode::Block);
  //       block $case0
  Instrs.emplace_back(OpCode::Block);
  
  //         local.get 0  (the switch index)
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  
  //         br_table $case0 $case1 $case2 $default
  Instrs.emplace_back(OpCode::Br_table);
  Instrs.back().setLabelListSize(4);  // 3 cases + default
  auto LabelList = Instrs.back().getLabelList();
  LabelList[0].TargetIndex = 0;  // case 0 -> $case0 (innermost)
  LabelList[1].TargetIndex = 1;  // case 1 -> $case1
  LabelList[2].TargetIndex = 2;  // case 2 -> $case2
  LabelList[3].TargetIndex = 3;  // default -> $default (outermost)
  
  //       end $case0
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(100));
  Instrs.emplace_back(OpCode::Return);
  
  //     end $case1
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(200));
  Instrs.emplace_back(OpCode::Return);
  
  //   end $case2
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(300));
  Instrs.emplace_back(OpCode::Return);
  
  // end $default
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::Return);
  
  // function end
  Instrs.emplace_back(OpCode::End);
  
  void* Func = buildCustomFunc(
    {ValType(TypeCode::I32)},
    {ValType(TypeCode::I32)},
    {},
    Instrs
  );
  
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)},
                             {ValType(TypeCode::I32)});
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 100);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(1u)}), 200);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(2u)}), 300);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(3u)}), 0);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(100u)}), 0);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(static_cast<uint32_t>(-1))}), 0);
}

// Test: br_table with single case (edge case)
TEST_F(IRExecutionTest, ControlFlow_BrTable_SingleCase) {
  std::vector<AST::Instruction> Instrs;
  
  // switch(x) { case 0: return 42; default: return 0; }
  
  // block $default
  Instrs.emplace_back(OpCode::Block);
  //   block $case0
  Instrs.emplace_back(OpCode::Block);
  
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  
  // br_table with 1 case + default
  Instrs.emplace_back(OpCode::Br_table);
  Instrs.back().setLabelListSize(2);
  auto LabelList = Instrs.back().getLabelList();
  LabelList[0].TargetIndex = 0;  // case 0 -> $case0
  LabelList[1].TargetIndex = 1;  // default -> $default
  
  //   end $case0
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(42));
  Instrs.emplace_back(OpCode::Return);
  
  // end $default
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(static_cast<int32_t>(0));
  Instrs.emplace_back(OpCode::Return);
  
  Instrs.emplace_back(OpCode::End);
  
  void* Func = buildCustomFunc(
    {ValType(TypeCode::I32)},
    {ValType(TypeCode::I32)},
    {},
    Instrs
  );
  
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)},
                             {ValType(TypeCode::I32)});
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 42);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(1u)}), 0);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(99u)}), 0);
}

//==============================================================================
// Function Call Execution Tests
//
// These tests verify that function calls work correctly.
// A mock "callee" function is created as a native C function, and the
// JIT-compiled "caller" function calls it through the function table.
//==============================================================================

// Native callee function: doubles its input
// JIT direct-call ABI: callee(env, args) with args[] = wasm args as uint64_t. Used in FuncTable.
extern "C" uint64_t native_double_jit(void* /*env*/, uint64_t* args) {
  int32_t x = static_cast<int32_t>(args[0] & 0xFFFFFFFFu);
  return static_cast<uint64_t>(static_cast<uint32_t>(x * 2));
}
extern "C" uint64_t native_add_jit(void* /*env*/, uint64_t* args) {
  int32_t a = static_cast<int32_t>(args[0] & 0xFFFFFFFFu);
  int32_t b = static_cast<int32_t>(args[1] & 0xFFFFFFFFu);
  return static_cast<uint64_t>(static_cast<uint32_t>(a + b));
}

// Test: Direct call to a native function
TEST_F(IRExecutionTest, Call_DirectToNative) {
  std::vector<AST::Instruction> Instrs;
  
  // Build caller function that calls func[0] with local.get 0
  // Caller signature: (i32) -> i32
  // Callee signature (func 0): (i32) -> i32
  
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;  // Push arg
  Instrs.emplace_back(OpCode::Call);
  Instrs.back().getTargetIndex() = 0;  // Call function 0
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::End);
  
  // Create function type for the callee
  AST::FunctionType CalleeType({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  std::vector<const AST::FunctionType*> ModuleFuncs = {&CalleeType};
  
  // Build the caller function
  Builder.reset();
  AST::FunctionType CallerType({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  ASSERT_TRUE(Builder.initialize(CallerType, {}));
  Builder.setModuleFunctions(ModuleFuncs);
  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));
  
  // Compile
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());
  ASSERT_TRUE(CompRes.has_value()) << "Failed to compile caller";
  void* Caller = CompRes->NativeFunc;
  ASSERT_NE(Caller, nullptr);
  
  // Set up function table with the JIT-ABI callee (env, uint64_t* args)
  void* FuncTable[1] = { reinterpret_cast<void*>(&native_double_jit) };

  std::vector<ValVariant> Args = {ValVariant(5u)};
  std::vector<ValVariant> Rets(1);
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 1,
                           nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 10);

  Args = {ValVariant(0u)};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 1,
                           nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 0);

  Args = {ValVariant(static_cast<uint32_t>(-7))};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 1,
                           nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), -14);

  Args = {ValVariant(100u)};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 1,
                           nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 200);
}

// Test: Call with multiple arguments
TEST_F(IRExecutionTest, Call_MultipleArgs) {
  std::vector<AST::Instruction> Instrs;
  
  // Build caller that calls add(a, b)
  // Caller signature: (i32, i32) -> i32
  // Callee signature (func 0): (i32, i32) -> i32
  
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;  // Push first arg
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;  // Push second arg
  Instrs.emplace_back(OpCode::Call);
  Instrs.back().getTargetIndex() = 0;  // Call function 0
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::End);
  
  // Create function type for the callee
  AST::FunctionType CalleeType({ValType(TypeCode::I32), ValType(TypeCode::I32)}, 
                               {ValType(TypeCode::I32)});
  std::vector<const AST::FunctionType*> ModuleFuncs = {&CalleeType};
  
  // Build the caller function
  Builder.reset();
  AST::FunctionType CallerType({ValType(TypeCode::I32), ValType(TypeCode::I32)}, 
                               {ValType(TypeCode::I32)});
  ASSERT_TRUE(Builder.initialize(CallerType, {}));
  Builder.setModuleFunctions(ModuleFuncs);
  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));
  
  // Compile
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());
  ASSERT_TRUE(CompRes.has_value()) << "Failed to compile caller";
  void* Caller = CompRes->NativeFunc;
  ASSERT_NE(Caller, nullptr);
  
  void* FuncTable[1] = { reinterpret_cast<void*>(&native_add_jit) };

  std::vector<ValVariant> Args = {ValVariant(3u), ValVariant(4u)};
  std::vector<ValVariant> Rets(1);
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 1,
                           nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 7);

  Args = {ValVariant(0u), ValVariant(0u)};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 1,
                           nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 0);

  Args = {ValVariant(static_cast<uint32_t>(-5)), ValVariant(10u)};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 1,
                           nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 5);

  Args = {ValVariant(100u), ValVariant(200u)};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 1,
                           nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 300);
}

extern "C" uint64_t native_triple_jit(void* /*env*/, uint64_t* args) {
  int32_t x = static_cast<int32_t>(args[0] & 0xFFFFFFFFu);
  return static_cast<uint64_t>(static_cast<uint32_t>(x * 3));
}

// Test: call_indirect - indirect call with runtime index
TEST_F(IRExecutionTest, CallIndirect_RuntimeIndex) {
  std::vector<AST::Instruction> Instrs;
  
  // Build caller that does call_indirect with index from local.get 1
  // Caller signature: (i32 value, i32 func_index) -> i32
  // The function table will have [native_double, native_triple]
  // Type 0: (i32) -> i32
  
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;  // Push value argument
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;  // Push function index
  Instrs.emplace_back(OpCode::Call_indirect);
  Instrs.back().getTargetIndex() = 0;  // Type index 0
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::End);
  
  // Type 0 is the signature of functions in the table: (i32) -> i32
  AST::FunctionType Type0({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  std::vector<const AST::FunctionType*> ModuleTypes = {&Type0};

  Builder.reset();
  AST::FunctionType CallerType({ValType(TypeCode::I32), ValType(TypeCode::I32)},
                               {ValType(TypeCode::I32)});
  ASSERT_TRUE(Builder.initialize(CallerType, {}));
  Builder.setModuleTypes(ModuleTypes);
  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));
  
  // Compile
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());
  ASSERT_TRUE(CompRes.has_value()) << "Failed to compile call_indirect function";
  void* Caller = CompRes->NativeFunc;
  ASSERT_NE(Caller, nullptr);
  
  void* FuncTable[2] = {
    reinterpret_cast<void*>(&native_double_jit),
    reinterpret_cast<void*>(&native_triple_jit)
  };

  std::vector<ValVariant> Args = {ValVariant(5u), ValVariant(0u)};
  std::vector<ValVariant> Rets(1);
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 2,
                            nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 10);

  Args = {ValVariant(5u), ValVariant(1u)};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 2,
                            nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 15);

  Args = {ValVariant(7u), ValVariant(0u)};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 2,
                            nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 14);

  Args = {ValVariant(7u), ValVariant(1u)};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 2,
                            nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 21);

  Args = {ValVariant(static_cast<uint32_t>(-4)), ValVariant(0u)};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 2,
                            nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), -8);

  Args = {ValVariant(static_cast<uint32_t>(-4)), ValVariant(1u)};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 2,
                            nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), -12);
}

// Test: call_indirect with constant index (known at compile time)
TEST_F(IRExecutionTest, CallIndirect_ConstantIndex) {
  std::vector<AST::Instruction> Instrs;
  
  // Build caller that does call_indirect with constant index 1
  // Caller signature: (i32 value) -> i32
  
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;  // Push value argument
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(1);  // Constant index 1
  Instrs.emplace_back(OpCode::Call_indirect);
  Instrs.back().getTargetIndex() = 0;  // Type index 0
  Instrs.emplace_back(OpCode::Return);
  Instrs.emplace_back(OpCode::End);
  
  AST::FunctionType Type0({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  std::vector<const AST::FunctionType*> ModuleTypes = {&Type0};

  Builder.reset();
  AST::FunctionType CallerType({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  ASSERT_TRUE(Builder.initialize(CallerType, {}));
  Builder.setModuleTypes(ModuleTypes);
  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));

  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());
  ASSERT_TRUE(CompRes.has_value()) << "Failed to compile";
  void* Caller = CompRes->NativeFunc;
  ASSERT_NE(Caller, nullptr);

  void* FuncTable[2] = {
    reinterpret_cast<void*>(&native_double_jit),
    reinterpret_cast<void*>(&native_triple_jit)
  };

  std::vector<ValVariant> Args = {ValVariant(5u)};
  std::vector<ValVariant> Rets(1);
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 2,
                            nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 15);

  Args = {ValVariant(10u)};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 2,
                            nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 30);

  Args = {ValVariant(static_cast<uint32_t>(-3))};
  ASSERT_TRUE(Engine.invoke(Caller, CallerType, Args, Rets, FuncTable, 2,
                            nullptr, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), -9);
}

//==============================================================================
// Global Operations Execution Tests
//
// These tests verify that global.get and global.set work correctly.
// Globals are stored as 8-byte slots in a global array.
//==============================================================================

// Test: global.get for i32 global
TEST_F(IRExecutionTest, Global_GetI32) {
  // Build: global.get 0 (returns the value of global 0)
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Global__get);
  Instrs.back().getTargetIndex() = 0;  // Get global 0
  Instrs.emplace_back(OpCode::End);
  
  Builder.reset();
  AST::FunctionType FuncType({}, {ValType(TypeCode::I32)});
  ASSERT_TRUE(Builder.initialize(FuncType, {}));
  
  // Set up global types
  std::vector<ValType> GlobalTypes = {ValType(TypeCode::I32)};
  Builder.setModuleGlobals(GlobalTypes);
  
  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));
  
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());
  ASSERT_TRUE(CompRes.has_value()) << "Failed to compile global.get";
  void* Func = CompRes->NativeFunc;
  ASSERT_NE(Func, nullptr);
  
  ValVariant GlobalVal;
  ValVariant* GlobalPtrs[1] = {&GlobalVal};

  std::vector<ValVariant> Args;
  std::vector<ValVariant> Rets(1);

  GlobalVal = ValVariant(static_cast<uint32_t>(42));
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 42);

  GlobalVal = ValVariant(static_cast<uint32_t>(-1));
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), -1);

  GlobalVal = ValVariant(static_cast<uint32_t>(0x7FFFFFFF));
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 0x7FFFFFFF);
}

// Test: global.set for i32 global
TEST_F(IRExecutionTest, Global_SetI32) {
  // Build: local.get 0, global.set 0 (set global 0 to the input value)
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;  // Push input parameter
  Instrs.emplace_back(OpCode::Global__set);
  Instrs.back().getTargetIndex() = 0;  // Set global 0
  Instrs.emplace_back(OpCode::End);
  
  Builder.reset();
  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {});  // (i32) -> ()
  ASSERT_TRUE(Builder.initialize(FuncType, {}));
  
  std::vector<ValType> GlobalTypes = {ValType(TypeCode::I32)};
  Builder.setModuleGlobals(GlobalTypes);
  
  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));
  
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());
  ASSERT_TRUE(CompRes.has_value()) << "Failed to compile global.set";
  void* Func = CompRes->NativeFunc;
  ASSERT_NE(Func, nullptr);
  
  ValVariant GlobalVal(static_cast<uint32_t>(0));
  ValVariant* GlobalPtrs[1] = {&GlobalVal};

  std::vector<ValVariant> Args = {ValVariant(123u)};
  std::vector<ValVariant> Rets;
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                             GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(GlobalVal.get<uint32_t>(), 123u);

  Args = {ValVariant(static_cast<uint32_t>(-999))};
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                             GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(GlobalVal.get<uint32_t>()), -999);

  Args = {ValVariant(0u)};
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                             GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(GlobalVal.get<uint32_t>(), 0u);
}

// Test: global get/set roundtrip - increment a global
TEST_F(IRExecutionTest, Global_Increment) {
  // Build: global.get 0, i32.const 1, i32.add, global.set 0, global.get 0
  // This increments global 0 and returns its new value
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Global__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(1);
  Instrs.emplace_back(OpCode::I32__add);
  Instrs.emplace_back(OpCode::Global__set);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Global__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::End);
  
  Builder.reset();
  AST::FunctionType FuncType({}, {ValType(TypeCode::I32)});  // () -> i32
  ASSERT_TRUE(Builder.initialize(FuncType, {}));
  
  std::vector<ValType> GlobalTypes = {ValType(TypeCode::I32)};
  Builder.setModuleGlobals(GlobalTypes);
  
  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));
  
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());
  ASSERT_TRUE(CompRes.has_value()) << "Failed to compile global increment";
  void* Func = CompRes->NativeFunc;
  ASSERT_NE(Func, nullptr);
  
  ValVariant GlobalVal(static_cast<uint32_t>(0));
  ValVariant* GlobalPtrs[1] = {&GlobalVal};

  std::vector<ValVariant> Args;
  std::vector<ValVariant> Rets(1);

  GlobalVal = ValVariant(static_cast<uint32_t>(0));
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 1);
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 2);
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 3);

  GlobalVal = ValVariant(static_cast<uint32_t>(100));
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 101);
}

// Test: multiple globals
TEST_F(IRExecutionTest, Global_Multiple) {
  // Build: global.get 0, global.get 1, i32.add (add two globals)
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Global__get);
  Instrs.back().getTargetIndex() = 0;  // Get global 0
  Instrs.emplace_back(OpCode::Global__get);
  Instrs.back().getTargetIndex() = 1;  // Get global 1
  Instrs.emplace_back(OpCode::I32__add);
  Instrs.emplace_back(OpCode::End);
  
  Builder.reset();
  AST::FunctionType FuncType({}, {ValType(TypeCode::I32)});
  ASSERT_TRUE(Builder.initialize(FuncType, {}));
  
  std::vector<ValType> GlobalTypes = {ValType(TypeCode::I32), ValType(TypeCode::I32)};
  Builder.setModuleGlobals(GlobalTypes);
  
  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));
  
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());
  ASSERT_TRUE(CompRes.has_value()) << "Failed to compile multiple globals";
  void* Func = CompRes->NativeFunc;
  ASSERT_NE(Func, nullptr);
  
  ValVariant GlobalVal0(static_cast<uint32_t>(0));
  ValVariant GlobalVal1(static_cast<uint32_t>(0));
  ValVariant* GlobalPtrs[2] = {&GlobalVal0, &GlobalVal1};

  std::vector<ValVariant> Args;
  std::vector<ValVariant> Rets(1);

  GlobalVal0 = ValVariant(static_cast<uint32_t>(10));
  GlobalVal1 = ValVariant(static_cast<uint32_t>(20));
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 30);

  GlobalVal0 = ValVariant(static_cast<uint32_t>(-5));
  GlobalVal1 = ValVariant(static_cast<uint32_t>(5));
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 0);

  GlobalVal0 = ValVariant(static_cast<uint32_t>(100));
  GlobalVal1 = ValVariant(static_cast<uint32_t>(200));
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), 300);
}

// Test: i64 global
TEST_F(IRExecutionTest, Global_I64) {
  // Build: global.get 0 (returns i64 global)
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Global__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::End);
  
  Builder.reset();
  AST::FunctionType FuncType({}, {ValType(TypeCode::I64)});
  ASSERT_TRUE(Builder.initialize(FuncType, {}));
  
  std::vector<ValType> GlobalTypes = {ValType(TypeCode::I64)};
  Builder.setModuleGlobals(GlobalTypes);
  
  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));
  
  IRJitEngine Engine;
  auto CompRes = Engine.compile(Builder.getIRContext());
  ASSERT_TRUE(CompRes.has_value()) << "Failed to compile i64 global.get";
  void* Func = CompRes->NativeFunc;
  ASSERT_NE(Func, nullptr);
  
  ValVariant GlobalVal(static_cast<uint64_t>(0));
  ValVariant* GlobalPtrs[1] = {&GlobalVal};

  std::vector<ValVariant> Args;
  std::vector<ValVariant> Rets(1);

  GlobalVal = ValVariant(static_cast<uint64_t>(0x123456789ABCDEF0ULL));
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int64_t>(Rets[0].get<uint64_t>()), 0x123456789ABCDEF0LL);

  GlobalVal = ValVariant(static_cast<uint64_t>(-1LL));
  ASSERT_TRUE(Engine.invoke(Func, FuncType, Args, Rets, nullptr, 0,
                            GlobalPtrs, Memory.data(), Memory.size()));
  EXPECT_EQ(static_cast<int64_t>(Rets[0].get<uint64_t>()), -1LL);
}

//==============================================================================
// Reference Types Execution Tests
//==============================================================================

// Test: ref.null (funcref), ref.is_null -> 1 (null ref is null)
TEST_F(IRExecutionTest, Ref_Null_IsNull_ReturnsOne) {
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Ref__null);
  Instrs.back().setValType(ValType(TypeCode::FuncRef));
  Instrs.emplace_back(OpCode::Ref__is_null);
  Instrs.emplace_back(OpCode::End);

  void* Func = buildCustomFunc({}, {ValType(TypeCode::I32)}, {}, Instrs);
  ASSERT_NE(Func, nullptr) << "Failed to compile ref.null + ref.is_null";
  EXPECT_EQ(execI32NoParams(Func), 1);
}

// Test: ref in block with result - ref.null in block, ref.is_null -> 1
TEST_F(IRExecutionTest, Ref_BlockResult_IsNull) {
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Block);
  Instrs.back().getBlockType() = BlockType(ValType(TypeCode::FuncRef));
  Instrs.emplace_back(OpCode::Ref__null);
  Instrs.back().setValType(ValType(TypeCode::FuncRef));
  Instrs.emplace_back(OpCode::End);
  Instrs.emplace_back(OpCode::Ref__is_null);
  Instrs.emplace_back(OpCode::End);

  void* Func = buildCustomFunc({}, {ValType(TypeCode::I32)}, {}, Instrs);
  ASSERT_NE(Func, nullptr) << "Failed to compile block (ref) + ref.is_null";
  EXPECT_EQ(execI32NoParams(Func), 1);
}

//==============================================================================
// Memory Operations Execution Tests
//
// These tests verify that memory load/store operations work correctly.
// The Memory buffer (65536 bytes) is pre-initialized with test values
// and operations are verified by checking loaded values match expected
// or stored values appear in memory.
//
// Test coverage:
// - i32.load / i32.store (full 32-bit)
// - i64.load / i64.store (full 64-bit)
// - i32.load8_s/u, i32.load16_s/u (partial loads with sign/zero extend)
// - i64.load8_s/u, i64.load16_s/u, i64.load32_s/u (partial loads)
// - i32.store8/16, i64.store8/16/32 (partial stores with truncation)
//==============================================================================

// Helper to build a memory load function
// Returns a function that: (address) -> loaded_value
class MemoryExecutionTest : public IRExecutionTest {
protected:
  void SetUp() override {
    // Initialize memory with known pattern for testing
    // Bytes 0-7: 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
    Memory[0] = 0x01; Memory[1] = 0x02; Memory[2] = 0x03; Memory[3] = 0x04;
    Memory[4] = 0x05; Memory[5] = 0x06; Memory[6] = 0x07; Memory[7] = 0x08;
    
    // Bytes 16-23: Store a known i32 (0x12345678) at offset 16
    Memory[16] = 0x78; Memory[17] = 0x56; Memory[18] = 0x34; Memory[19] = 0x12;
    
    // Bytes 24-31: Store a known i64 (0xFEDCBA9876543210) at offset 24
    Memory[24] = 0x10; Memory[25] = 0x32; Memory[26] = 0x54; Memory[27] = 0x76;
    Memory[28] = 0x98; Memory[29] = 0xBA; Memory[30] = 0xDC; Memory[31] = 0xFE;
    
    // Bytes 32-35: Negative i32 (-1 = 0xFFFFFFFF) for sign extension tests
    Memory[32] = 0xFF; Memory[33] = 0xFF; Memory[34] = 0xFF; Memory[35] = 0xFF;
    
    // Bytes 40-41: Negative i16 (-1 = 0xFFFF) at offset 40
    Memory[40] = 0xFF; Memory[41] = 0xFF;
    
    // Byte 48: Negative i8 (-1 = 0xFF) at offset 48
    Memory[48] = 0xFF;
    
    // Byte 50: Value 0x80 (128 unsigned, -128 signed) at offset 50
    Memory[50] = 0x80;
  }
  
  // Build a load function: load from memory[addr] and return
  void* buildLoadFunc(OpCode LoadOp, TypeCode RetType) {
    Builder.reset();
    
    std::vector<ValType> ParamTypes = {ValType(TypeCode::I32)}; // address param
    std::vector<ValType> RetTypes = {ValType(RetType)};
    AST::FunctionType FuncType(ParamTypes, RetTypes);
    
    auto InitRes = Builder.initialize(FuncType, {});
    if (!InitRes) return nullptr;
    
    std::vector<AST::Instruction> Instrs;
    // Load from memory[param0]
    Instrs.emplace_back(OpCode::Local__get);
    Instrs.back().getTargetIndex() = 0;
    Instrs.emplace_back(LoadOp);
    Instrs.back().getMemoryOffset() = 0;
    Instrs.back().getMemoryAlign() = 0;
    Instrs.emplace_back(OpCode::End);
    
    auto BuildRes = Builder.buildFromInstructions(Instrs);
    if (!BuildRes) return nullptr;
    
    auto CompRes = Engine.compile(Builder.getIRContext());
    if (!CompRes) return nullptr;
    
    return CompRes->NativeFunc;
  }
  
  // Build a store function: store value to memory[addr]
  void* buildStoreFunc(OpCode StoreOp, TypeCode ValType_) {
    Builder.reset();
    
    // Parameters: (address: i32, value: ValType_)
    std::vector<ValType> ParamTypes = {ValType(TypeCode::I32), ValType(ValType_)};
    std::vector<ValType> RetTypes = {};  // void return
    AST::FunctionType FuncType(ParamTypes, RetTypes);
    
    auto InitRes = Builder.initialize(FuncType, {});
    if (!InitRes) return nullptr;
    
    std::vector<AST::Instruction> Instrs;
    // Store param1 to memory[param0]
    Instrs.emplace_back(OpCode::Local__get);
    Instrs.back().getTargetIndex() = 0;  // address
    Instrs.emplace_back(OpCode::Local__get);
    Instrs.back().getTargetIndex() = 1;  // value
    Instrs.emplace_back(StoreOp);
    Instrs.back().getMemoryOffset() = 0;
    Instrs.back().getMemoryAlign() = 0;
    Instrs.emplace_back(OpCode::End);
    
    auto BuildRes = Builder.buildFromInstructions(Instrs);
    if (!BuildRes) return nullptr;
    
    auto CompRes = Engine.compile(Builder.getIRContext());
    if (!CompRes) return nullptr;
    
    return CompRes->NativeFunc;
  }
};

//------------------------------------------------------------------------------
// i32 Load Tests
//------------------------------------------------------------------------------

TEST_F(MemoryExecutionTest, Memory_I32_Load) {
  void* Func = buildLoadFunc(OpCode::I32__load, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(16u)}), 0x12345678);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 0x04030201);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(32u)}), -1);
}

TEST_F(MemoryExecutionTest, Memory_I32_Load8_S) {
  void* Func = buildLoadFunc(OpCode::I32__load8_s, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 1);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(48u)}), -1);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(50u)}), -128);
}

TEST_F(MemoryExecutionTest, Memory_I32_Load8_U) {
  void* Func = buildLoadFunc(OpCode::I32__load8_u, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 1);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(48u)}), 255);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(50u)}), 128);
}

TEST_F(MemoryExecutionTest, Memory_I32_Load16_S) {
  void* Func = buildLoadFunc(OpCode::I32__load16_s, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 0x0201);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(40u)}), -1);
}

TEST_F(MemoryExecutionTest, Memory_I32_Load16_U) {
  void* Func = buildLoadFunc(OpCode::I32__load16_u, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(0u)}), 0x0201);
  EXPECT_EQ(invokeI32(Func, FuncType, {ValVariant(40u)}), 65535);
}

//------------------------------------------------------------------------------
// i64 Load Tests
//------------------------------------------------------------------------------

TEST_F(MemoryExecutionTest, Memory_I64_Load) {
  void* Func = buildLoadFunc(OpCode::I64__load, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {ValType(TypeCode::I64)});
  EXPECT_EQ(invokeI64(Func, FuncType, {ValVariant(24u)}), static_cast<int64_t>(0xFEDCBA9876543210ULL));
  EXPECT_EQ(invokeI64(Func, FuncType, {ValVariant(0u)}), static_cast<int64_t>(0x0807060504030201ULL));
}

TEST_F(MemoryExecutionTest, Memory_I64_Load8_S) {
  void* Func = buildLoadFunc(OpCode::I64__load8_s, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {ValType(TypeCode::I64)});
  EXPECT_EQ(invokeI64(Func, FuncType, {ValVariant(0u)}), 1LL);
  EXPECT_EQ(invokeI64(Func, FuncType, {ValVariant(48u)}), -1LL);
}

TEST_F(MemoryExecutionTest, Memory_I64_Load8_U) {
  void* Func = buildLoadFunc(OpCode::I64__load8_u, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {ValType(TypeCode::I64)});
  EXPECT_EQ(invokeI64(Func, FuncType, {ValVariant(0u)}), 1LL);
  EXPECT_EQ(invokeI64(Func, FuncType, {ValVariant(48u)}), 255LL);
}

TEST_F(MemoryExecutionTest, Memory_I64_Load32_S) {
  void* Func = buildLoadFunc(OpCode::I64__load32_s, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {ValType(TypeCode::I64)});
  EXPECT_EQ(invokeI64(Func, FuncType, {ValVariant(16u)}), 0x12345678LL);
  EXPECT_EQ(invokeI64(Func, FuncType, {ValVariant(32u)}), -1LL);
}

TEST_F(MemoryExecutionTest, Memory_I64_Load32_U) {
  void* Func = buildLoadFunc(OpCode::I64__load32_u, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {ValType(TypeCode::I64)});
  EXPECT_EQ(invokeI64(Func, FuncType, {ValVariant(16u)}), 0x12345678LL);
  EXPECT_EQ(invokeI64(Func, FuncType, {ValVariant(32u)}), 0xFFFFFFFFLL);
}

//------------------------------------------------------------------------------
// i32 Store Tests
//------------------------------------------------------------------------------

TEST_F(MemoryExecutionTest, Memory_I32_Store) {
  void* Func = buildStoreFunc(OpCode::I32__store, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32), ValType(TypeCode::I32)}, {});
  invokeVoid(Func, FuncType, {ValVariant(100u), ValVariant(static_cast<uint32_t>(0xDEADBEEF))});
  EXPECT_EQ(Memory[100], 0xEF);
  EXPECT_EQ(Memory[101], 0xBE);
  EXPECT_EQ(Memory[102], 0xAD);
  EXPECT_EQ(Memory[103], 0xDE);
}

TEST_F(MemoryExecutionTest, Memory_I32_Store8) {
  void* Func = buildStoreFunc(OpCode::I32__store8, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32), ValType(TypeCode::I32)}, {});
  invokeVoid(Func, FuncType, {ValVariant(110u), ValVariant(0x12345678u)});
  EXPECT_EQ(Memory[110], 0x78);
  invokeVoid(Func, FuncType, {ValVariant(111u), ValVariant(static_cast<uint32_t>(-1))});
  EXPECT_EQ(Memory[111], 0xFF);
}

TEST_F(MemoryExecutionTest, Memory_I32_Store16) {
  void* Func = buildStoreFunc(OpCode::I32__store16, TypeCode::I32);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32), ValType(TypeCode::I32)}, {});
  invokeVoid(Func, FuncType, {ValVariant(120u), ValVariant(0x12345678u)});
  EXPECT_EQ(Memory[120], 0x78);
  EXPECT_EQ(Memory[121], 0x56);
  invokeVoid(Func, FuncType, {ValVariant(122u), ValVariant(static_cast<uint32_t>(-1))});
  EXPECT_EQ(Memory[122], 0xFF);
  EXPECT_EQ(Memory[123], 0xFF);
}

//------------------------------------------------------------------------------
// i64 Store Tests
//------------------------------------------------------------------------------

TEST_F(MemoryExecutionTest, Memory_I64_Store) {
  void* Func = buildStoreFunc(OpCode::I64__store, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  invokeVoid(Func, FuncType, {ValVariant(200u), ValVariant(static_cast<uint64_t>(0x123456789ABCDEF0ULL))});
  EXPECT_EQ(Memory[200], 0xF0);
  EXPECT_EQ(Memory[201], 0xDE);
  EXPECT_EQ(Memory[202], 0xBC);
  EXPECT_EQ(Memory[203], 0x9A);
  EXPECT_EQ(Memory[204], 0x78);
  EXPECT_EQ(Memory[205], 0x56);
  EXPECT_EQ(Memory[206], 0x34);
  EXPECT_EQ(Memory[207], 0x12);
}

TEST_F(MemoryExecutionTest, Memory_I64_Store8) {
  void* Func = buildStoreFunc(OpCode::I64__store8, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  invokeVoid(Func, FuncType, {ValVariant(210u), ValVariant(static_cast<uint64_t>(0x123456789ABCDEF0ULL))});
  EXPECT_EQ(Memory[210], 0xF0);
}

TEST_F(MemoryExecutionTest, Memory_I64_Store16) {
  void* Func = buildStoreFunc(OpCode::I64__store16, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  invokeVoid(Func, FuncType, {ValVariant(220u), ValVariant(static_cast<uint64_t>(0x123456789ABCDEF0ULL))});
  EXPECT_EQ(Memory[220], 0xF0);
  EXPECT_EQ(Memory[221], 0xDE);
}

TEST_F(MemoryExecutionTest, Memory_I64_Store32) {
  void* Func = buildStoreFunc(OpCode::I64__store32, TypeCode::I64);
  ASSERT_NE(Func, nullptr);

  AST::FunctionType FuncType({ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  invokeVoid(Func, FuncType, {ValVariant(230u), ValVariant(static_cast<uint64_t>(0x123456789ABCDEF0ULL))});
  EXPECT_EQ(Memory[230], 0xF0);
  EXPECT_EQ(Memory[231], 0xDE);
  EXPECT_EQ(Memory[232], 0xBC);
  EXPECT_EQ(Memory[233], 0x9A);
}

//------------------------------------------------------------------------------
// Load/Store Round-Trip Tests
//------------------------------------------------------------------------------

TEST_F(MemoryExecutionTest, Memory_I32_RoundTrip) {
  void* StoreFunc = buildStoreFunc(OpCode::I32__store, TypeCode::I32);
  void* LoadFunc = buildLoadFunc(OpCode::I32__load, TypeCode::I32);
  ASSERT_NE(StoreFunc, nullptr);
  ASSERT_NE(LoadFunc, nullptr);

  AST::FunctionType StoreType({ValType(TypeCode::I32), ValType(TypeCode::I32)}, {});
  AST::FunctionType LoadType({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  int32_t testValues[] = {0, 1, -1, INT32_MAX, INT32_MIN, 0x12345678};
  for (int32_t val : testValues) {
    invokeVoid(StoreFunc, StoreType, {ValVariant(300u), ValVariant(static_cast<uint32_t>(val))});
    EXPECT_EQ(invokeI32(LoadFunc, LoadType, {ValVariant(300u)}), val) << "Round-trip failed for " << val;
  }
}

TEST_F(MemoryExecutionTest, Memory_I64_RoundTrip) {
  void* StoreFunc = buildStoreFunc(OpCode::I64__store, TypeCode::I64);
  void* LoadFunc = buildLoadFunc(OpCode::I64__load, TypeCode::I64);
  ASSERT_NE(StoreFunc, nullptr);
  ASSERT_NE(LoadFunc, nullptr);

  AST::FunctionType StoreType({ValType(TypeCode::I32), ValType(TypeCode::I64)}, {});
  AST::FunctionType LoadType({ValType(TypeCode::I32)}, {ValType(TypeCode::I64)});
  int64_t testValues[] = {0LL, 1LL, -1LL, INT64_MAX, INT64_MIN, 0x123456789ABCDEF0LL};
  for (int64_t val : testValues) {
    invokeVoid(StoreFunc, StoreType, {ValVariant(400u), ValVariant(static_cast<uint64_t>(val))});
    EXPECT_EQ(invokeI64(LoadFunc, LoadType, {ValVariant(400u)}), val) << "Round-trip failed for " << val;
  }
}

} // namespace VM
} // namespace WasmEdge

//==============================================================================
// Main Entry Point
//==============================================================================

int main(int argc, char **argv) {
  std::cout << "\n";
  std::cout << "========================================\n";
  std::cout << "WasmEdge IR JIT Execution Correctness Tests\n";
  std::cout << "Testing ACTUAL COMPUTED RESULTS\n";
  std::cout << "========================================\n";
  std::cout << "\n";

  ::testing::InitGoogleTest(&argc, argv);
  int result = RUN_ALL_TESTS();

  std::cout << "\n";
  if (result == 0) {
    std::cout << "========================================\n";
    std::cout << "All execution tests passed! ✅\n";
    std::cout << "JIT-compiled code produces correct results.\n";
    std::cout << "========================================\n";
  } else {
    std::cout << "========================================\n";
    std::cout << "Some execution tests FAILED! ❌\n";
    std::cout << "JIT-compiled code may produce incorrect results.\n";
    std::cout << "========================================\n";
  }
  std::cout << "\n";

  return result;
}
