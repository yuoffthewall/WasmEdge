// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- ir_integration_test.cpp - IR JIT Integration Tests ----------------===//
//
// Part of the WasmEdge Project.
//
// Tests the full integration of IR JIT with WasmEdge runtime.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#ifdef WASMEDGE_BUILD_IR_JIT

#include "vm/ir_builder.h"
#include "vm/ir_jit_engine.h"
#include "runtime/instance/function.h"
#include "runtime/instance/module.h"
#include "runtime/instance/memory.h"
#include "runtime/instance/global.h"
#include "executor/executor.h"
#include "runtime/stackmgr.h"
#include "ast/type.h"
#include "common/types.h"

extern "C" {
#include "ir.h"
}

#include <memory>
#include <vector>

using namespace WasmEdge;
using namespace WasmEdge::VM;
using namespace WasmEdge::Runtime;

class IRIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Initialize IR JIT engine
    Engine = std::make_unique<IRJitEngine>();
  }

  void TearDown() override {
    Engine.reset();
  }

  // Helper: Build IR from instructions
  bool buildIR(const AST::FunctionType &FuncType,
               const std::vector<AST::Instruction> &Instrs,
               Span<const std::pair<uint32_t, ValType>> Locals = {}) {
    Builder.reset();
    if (!Builder.initialize(FuncType, Locals)) {
      return false;
    }
    auto Result = Builder.buildFromInstructions(Instrs);
    return Result.has_value();
  }

  // Helper: Compile to native code
  IRJitEngine::CompileResult* compileToNative() {
    auto Result = Engine->compile(Builder.getIRContext());
    if (!Result) return nullptr;
    LastCompileResult = *Result;
    return &LastCompileResult;
  }

  WasmToIRBuilder Builder;
  std::unique_ptr<IRJitEngine> Engine;
  IRJitEngine::CompileResult LastCompileResult;
};

//==============================================================================
// Basic Integration Tests
//==============================================================================

TEST_F(IRIntegrationTest, SimpleAdd_ThroughInvoke) {
  // Build: local.get 0, local.get 1, i32.add
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__add);
  Instrs.emplace_back(OpCode::End);

  AST::FunctionType FuncType({ValType(TypeCode::I32), ValType(TypeCode::I32)},
                             {ValType(TypeCode::I32)});
  ASSERT_TRUE(buildIR(FuncType, Instrs));

  auto *Result = compileToNative();
  ASSERT_NE(Result, nullptr);
  ASSERT_NE(Result->NativeFunc, nullptr);

  // Invoke through IRJitEngine::invoke
  std::vector<ValVariant> Args = {ValVariant(uint32_t(10)), ValVariant(uint32_t(20))};
  std::vector<ValVariant> Rets(1);
  
  auto InvokeRes = Engine->invoke(Result->NativeFunc, FuncType, Args, Rets,
                                  nullptr, 0,   // func_table
                                  nullptr,      // global_base
                                  nullptr, 0);  // memory
  ASSERT_TRUE(InvokeRes.has_value());
  EXPECT_EQ(Rets[0].get<uint32_t>(), 30u);
}

TEST_F(IRIntegrationTest, SimpleMultiply) {
  // Build: local.get 0, local.get 1, i32.mul
  std::vector<AST::Instruction> Instrs;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__mul);
  Instrs.emplace_back(OpCode::End);

  AST::FunctionType FuncType({ValType(TypeCode::I32), ValType(TypeCode::I32)},
                             {ValType(TypeCode::I32)});
  ASSERT_TRUE(buildIR(FuncType, Instrs));

  auto *Result = compileToNative();
  ASSERT_NE(Result, nullptr);

  // Test multiplication
  auto test = [&](int32_t a, int32_t b, int32_t expected) {
    std::vector<ValVariant> Args = {ValVariant(uint32_t(a)), ValVariant(uint32_t(b))};
    std::vector<ValVariant> Rets(1);
    auto Res = Engine->invoke(Result->NativeFunc, FuncType, Args, Rets,
                              nullptr, 0, nullptr, nullptr, 0);
    ASSERT_TRUE(Res.has_value());
    EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), expected);
  };

  test(3, 4, 12);
  test(7, 8, 56);
  test(-5, 3, -15);
  test(0, 100, 0);
}

TEST_F(IRIntegrationTest, Factorial_Iterative) {
  // Build factorial function using iterative approach:
  // func(n: i32) -> i32 {
  //   local result = 1;
  //   block { loop {
  //     br_if 1 (n <= 1)  // exit if n <= 1
  //     result = result * n
  //     n = n - 1
  //     br 0              // continue loop
  //   } }
  //   return result
  // }
  std::vector<AST::Instruction> Instrs;
  
  // Initialize result (local 1) to 1
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(1);
  Instrs.emplace_back(OpCode::Local__set);
  Instrs.back().getTargetIndex() = 1;  // result
  
  // Outer block
  Instrs.emplace_back(OpCode::Block);
  
  // Loop
  Instrs.emplace_back(OpCode::Loop);
  
  // Check if n <= 1, exit if so
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;  // get n
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(1);
  Instrs.emplace_back(OpCode::I32__le_s);  // n <= 1
  Instrs.emplace_back(OpCode::Br_if);
  Instrs.back().getJump().TargetIndex = 1;  // exit to outer block
  
  // result = result * n
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;  // result
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;  // n
  Instrs.emplace_back(OpCode::I32__mul);
  Instrs.emplace_back(OpCode::Local__set);
  Instrs.back().getTargetIndex() = 1;  // result
  
  // n = n - 1
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;  
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(1);
  Instrs.emplace_back(OpCode::I32__sub);
  Instrs.emplace_back(OpCode::Local__set);
  Instrs.back().getTargetIndex() = 0;
  
  // Continue loop
  Instrs.emplace_back(OpCode::Br);
  Instrs.back().getJump().TargetIndex = 0;  // back to loop start
  
  Instrs.emplace_back(OpCode::End);  // end loop
  Instrs.emplace_back(OpCode::End);  // end block
  
  // Return result
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::End);  // end func

  AST::FunctionType FuncType({ValType(TypeCode::I32)}, {ValType(TypeCode::I32)});
  std::vector<std::pair<uint32_t, ValType>> Locals = {{1, ValType(TypeCode::I32)}};
  
  ASSERT_TRUE(buildIR(FuncType, Instrs, Locals));

  auto *Result = compileToNative();
  ASSERT_NE(Result, nullptr);

  // Test factorial values
  auto test = [&](int32_t n, int32_t expected) {
    std::vector<ValVariant> Args = {ValVariant(uint32_t(n))};
    std::vector<ValVariant> Rets(1);
    auto Res = Engine->invoke(Result->NativeFunc, FuncType, Args, Rets,
                              nullptr, 0, nullptr, nullptr, 0);
    ASSERT_TRUE(Res.has_value()) << "Failed for n=" << n;
    EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), expected) 
        << "Failed for n=" << n;
  };

  test(0, 1);    // 0! = 1
  test(1, 1);    // 1! = 1
  test(2, 2);    // 2! = 2
  test(3, 6);    // 3! = 6
  test(5, 120);  // 5! = 120
  test(10, 3628800);  // 10! = 3628800
}

TEST_F(IRIntegrationTest, MemoryAccess_StoreLoad) {
  // Build: store value at offset, then load and return it
  // func(offset: i32, value: i32) -> i32 {
  //   i32.store(offset, value)
  //   return i32.load(offset)
  // }
  
  std::vector<AST::Instruction> Instrs;
  
  // i32.store(offset, value)
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;  // offset
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;  // value
  Instrs.emplace_back(OpCode::I32__store);
  Instrs.back().getMemoryOffset() = 0;
  Instrs.back().getMemoryAlign() = 2;  // 4-byte aligned
  
  // i32.load(offset)
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;  // offset
  Instrs.emplace_back(OpCode::I32__load);
  Instrs.back().getMemoryOffset() = 0;
  Instrs.back().getMemoryAlign() = 2;
  
  Instrs.emplace_back(OpCode::End);

  AST::FunctionType FuncType({ValType(TypeCode::I32), ValType(TypeCode::I32)},
                             {ValType(TypeCode::I32)});
  ASSERT_TRUE(buildIR(FuncType, Instrs));

  auto *Result = compileToNative();
  ASSERT_NE(Result, nullptr);

  // Allocate memory buffer
  std::vector<uint8_t> Memory(65536, 0);

  // Test store/load roundtrip
  auto testRoundtrip = [&](int32_t offset, int32_t value) {
    std::vector<ValVariant> Args = {ValVariant(uint32_t(offset)), ValVariant(uint32_t(value))};
    std::vector<ValVariant> Rets(1);
    auto Res = Engine->invoke(Result->NativeFunc, FuncType, Args, Rets,
                              nullptr, 0, nullptr, Memory.data(), Memory.size());
    ASSERT_TRUE(Res.has_value());
    EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), value)
        << "Store/load roundtrip failed for value " << value;
  };

  testRoundtrip(0, 42);
  testRoundtrip(100, -1);
  testRoundtrip(1000, 0x12345678);
  testRoundtrip(4, 0);
}

TEST_F(IRIntegrationTest, GlobalAccess_IncrementCounter) {
  // Build: increment global counter and return new value
  // global $counter: mut i32
  // func() -> i32 {
  //   global.set 0 (global.get 0 + 1)
  //   return global.get 0
  // }
  
  std::vector<AST::Instruction> Instrs;
  
  // global.set 0 (global.get 0 + 1)
  Instrs.emplace_back(OpCode::Global__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(1);
  Instrs.emplace_back(OpCode::I32__add);
  Instrs.emplace_back(OpCode::Global__set);
  Instrs.back().getTargetIndex() = 0;
  
  // return global.get 0
  Instrs.emplace_back(OpCode::Global__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::End);

  AST::FunctionType FuncType({}, {ValType(TypeCode::I32)});
  
  Builder.reset();
  ASSERT_TRUE(Builder.initialize(FuncType, {}));
  
  // Set global types
  std::vector<ValType> GlobalTypes = {ValType(TypeCode::I32)};
  Builder.setModuleGlobals(GlobalTypes);
  
  ASSERT_TRUE(Builder.buildFromInstructions(Instrs));

  auto *Result = compileToNative();
  ASSERT_NE(Result, nullptr);

  // Set up globals array (8 bytes per global)
  int64_t Globals[1] = {0};

  // Call multiple times, each should increment
  for (int expected = 1; expected <= 5; expected++) {
    std::vector<ValVariant> Args;
    std::vector<ValVariant> Rets(1);
    auto Res = Engine->invoke(Result->NativeFunc, FuncType, Args, Rets,
                              nullptr, 0, Globals, nullptr, 0);
    ASSERT_TRUE(Res.has_value());
    EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), expected)
        << "Counter should be " << expected;
  }
}

TEST_F(IRIntegrationTest, ConditionalLogic_Max) {
  // Build: max(a, b) using if/else with explicit returns
  // if (a > b) return a else return b
  std::vector<AST::Instruction> Instrs;
  
  // if (a > b)
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::I32__gt_s);
  Instrs.emplace_back(OpCode::If);
  
  // then: return a
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 0;
  Instrs.emplace_back(OpCode::Return);
  
  Instrs.emplace_back(OpCode::Else);
  
  // else: return b
  Instrs.emplace_back(OpCode::Local__get);
  Instrs.back().getTargetIndex() = 1;
  Instrs.emplace_back(OpCode::Return);
  
  Instrs.emplace_back(OpCode::End);  // end if
  
  // Unreachable fallthrough (should never execute)
  Instrs.emplace_back(OpCode::I32__const);
  Instrs.back().setNum(0);
  Instrs.emplace_back(OpCode::End);  // end func

  AST::FunctionType FuncType({ValType(TypeCode::I32), ValType(TypeCode::I32)},
                             {ValType(TypeCode::I32)});
  ASSERT_TRUE(buildIR(FuncType, Instrs));

  auto *Result = compileToNative();
  ASSERT_NE(Result, nullptr);

  auto testMax = [&](int32_t a, int32_t b, int32_t expected) {
    std::vector<ValVariant> Args = {ValVariant(uint32_t(a)), ValVariant(uint32_t(b))};
    std::vector<ValVariant> Rets(1);
    auto Res = Engine->invoke(Result->NativeFunc, FuncType, Args, Rets,
                              nullptr, 0, nullptr, nullptr, 0);
    ASSERT_TRUE(Res.has_value());
    EXPECT_EQ(static_cast<int32_t>(Rets[0].get<uint32_t>()), expected)
        << "max(" << a << ", " << b << ") should be " << expected;
  };

  testMax(10, 5, 10);
  testMax(3, 7, 7);
  testMax(-5, -10, -5);
  testMax(0, 0, 0);
  testMax(-1, 1, 1);
}

#endif // WASMEDGE_BUILD_IR_JIT

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
