// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- ir_e2e_test.cpp - IR JIT End-to-End Tests -------------------------===//
//
// Part of the WasmEdge Project.
//
// Tests the full integration of IR JIT with actual .wasm files.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#ifdef WASMEDGE_BUILD_IR_JIT

#include "common/configure.h"
#include "loader/loader.h"
#include "validator/validator.h"
#include "executor/executor.h"
#include "runtime/storemgr.h"
#include "ast/module.h"

#include <filesystem>
#include <memory>
#include <vector>

using namespace WasmEdge;

class IRE2ETest : public ::testing::Test {
protected:
  void SetUp() override {
    // Enable IR JIT in configuration
    Conf.getCompilerConfigure().setOptimizationLevel(
        CompilerConfigure::OptimizationLevel::O0);
  }

  Configure Conf;
};

// Helper to find test data directory
std::filesystem::path getTestDataPath() {
  // Try different possible locations
  std::vector<std::filesystem::path> Candidates = {
    std::filesystem::path(__FILE__).parent_path() / "testdata",
    std::filesystem::current_path() / "test/ir/testdata",
    std::filesystem::current_path() / "../test/ir/testdata",
  };
  
  for (const auto &Path : Candidates) {
    if (std::filesystem::exists(Path)) {
      return Path;
    }
  }
  
  return Candidates[0];  // Default
}

TEST_F(IRE2ETest, LoadAndRunFactorial) {
  // This test loads a .wasm file, compiles with IR JIT,
  // and runs the factorial function.
  
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "factorial.wasm";
  
  // Skip if test data not found
  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found: " << WasmPath;
  }
  
  // Create loader and load the module
  Loader::Loader Loader(Conf);
  
  // Load from .wasm binary
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value()) << "Failed to load WAT file";
  
  auto Mod = std::move(*LoadRes);
  
  // Validate the module
  Validator::Validator Validator(Conf);
  auto ValidRes = Validator.validate(*Mod);
  ASSERT_TRUE(ValidRes.has_value()) << "Failed to validate module";
  
  // Create executor and store
  Runtime::StoreManager StoreMgr;
  Executor::Executor Executor(Conf);
  
  // Instantiate the module
  auto InstRes = Executor.registerModule(StoreMgr, *Mod, "test");
  ASSERT_TRUE(InstRes.has_value()) << "Failed to instantiate module";
  
  auto *ModInst = InstRes->get();
  ASSERT_NE(ModInst, nullptr);
  
  // Find and call the factorial function
  auto *FuncInst = ModInst->findFuncExports("factorial");
  ASSERT_NE(FuncInst, nullptr) << "factorial function not found";
  
  // Check if the function was IR JIT compiled
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Function should be IR JIT compiled";
  
  // Get param types from function type
  const auto &FuncType = FuncInst->getFuncType();
  auto ParamTypes = FuncType.getParamTypes();
  
  // Test factorial(5) = 120
  {
    std::vector<ValVariant> Args = {ValVariant(uint32_t(5))};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value()) << "Failed to execute factorial(5)";
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 120u) << "factorial(5) should be 120";
  }
  
  // Test factorial(0) = 1
  {
    std::vector<ValVariant> Args = {ValVariant(uint32_t(0))};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value()) << "Failed to execute factorial(0)";
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 1u) << "factorial(0) should be 1";
  }
  
  // Test factorial(10) = 3628800
  {
    std::vector<ValVariant> Args = {ValVariant(uint32_t(10))};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value()) << "Failed to execute factorial(10)";
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 3628800u) << "factorial(10) should be 3628800";
  }
}

TEST_F(IRE2ETest, LoadAndRunAdd) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "factorial.wasm";
  
  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }
  
  Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());
  
  Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());
  
  Runtime::StoreManager StoreMgr;
  Executor::Executor Executor(Conf);
  
  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "test_add");
  ASSERT_TRUE(InstRes.has_value());
  
  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("add");
  ASSERT_NE(FuncInst, nullptr);
  
  EXPECT_TRUE(FuncInst->isIRJitFunction());
  
  // Get param types
  auto ParamTypes = FuncInst->getFuncType().getParamTypes();
  
  // Test add(10, 20) = 30
  std::vector<ValVariant> Args = {ValVariant(uint32_t(10)), ValVariant(uint32_t(20))};
  auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
  ASSERT_TRUE(ExecRes.has_value());
  ASSERT_EQ(ExecRes->size(), 1u);
  EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 30u);
}

TEST_F(IRE2ETest, LoadAndRunWithFunctionCall) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "factorial.wasm";
  
  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }
  
  Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());
  
  Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());
  
  Runtime::StoreManager StoreMgr;
  Executor::Executor Executor(Conf);
  
  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "test_call");
  ASSERT_TRUE(InstRes.has_value());
  
  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("factorial_plus_one");
  ASSERT_NE(FuncInst, nullptr);
  
  // Function with call instruction - tests inter-function JIT calls
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "factorial_plus_one should be JIT compiled";
  
  auto ParamTypes = FuncInst->getFuncType().getParamTypes();
  
  // Test factorial_plus_one(5) = factorial(5) + 1 = 121
  std::vector<ValVariant> Args = {ValVariant(uint32_t(5))};
  auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
  ASSERT_TRUE(ExecRes.has_value());
  ASSERT_EQ(ExecRes->size(), 1u);
  EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 121u) << "factorial_plus_one(5) should be 121";
}

TEST_F(IRE2ETest, LoadAndRunMemoryOps) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "factorial.wasm";
  
  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }
  
  Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());
  
  Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());
  
  Runtime::StoreManager StoreMgr;
  Executor::Executor Executor(Conf);
  
  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "test_mem");
  ASSERT_TRUE(InstRes.has_value());
  
  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("store_and_load");
  ASSERT_NE(FuncInst, nullptr);
  
  EXPECT_TRUE(FuncInst->isIRJitFunction());
  
  auto ParamTypes = FuncInst->getFuncType().getParamTypes();
  
  // Test store_and_load(0, 42) = 42
  std::vector<ValVariant> Args = {ValVariant(uint32_t(0)), ValVariant(uint32_t(42))};
  auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
  ASSERT_TRUE(ExecRes.has_value());
  ASSERT_EQ(ExecRes->size(), 1u);
  EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 42u);
}

TEST_F(IRE2ETest, LoadAndRunGlobalOps) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "factorial.wasm";
  
  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }
  
  Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());
  
  Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());
  
  Runtime::StoreManager StoreMgr;
  Executor::Executor Executor(Conf);
  
  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "test_global");
  ASSERT_TRUE(InstRes.has_value());
  
  auto *ModInst = InstRes->get();
  
  auto *GetCounterFunc = ModInst->findFuncExports("get_counter");
  auto *IncrementFunc = ModInst->findFuncExports("increment_counter");
  ASSERT_NE(GetCounterFunc, nullptr);
  ASSERT_NE(IncrementFunc, nullptr);
  
  // Global functions should be JIT compiled
  EXPECT_TRUE(GetCounterFunc->isIRJitFunction()) << "get_counter should be JIT compiled";
  EXPECT_TRUE(IncrementFunc->isIRJitFunction()) << "increment_counter should be JIT compiled";
  
  auto GetParamTypes = GetCounterFunc->getFuncType().getParamTypes();
  auto IncrParamTypes = IncrementFunc->getFuncType().getParamTypes();
  
  // Test get_counter() = 0 initially
  {
    std::vector<ValVariant> Args;
    auto ExecRes = Executor.invoke(GetCounterFunc, Args, GetParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 0u);
  }
  
  // Test increment_counter() = 1
  {
    std::vector<ValVariant> Args;
    auto ExecRes = Executor.invoke(IncrementFunc, Args, IncrParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 1u);
  }
  
  // Test increment_counter() = 2
  {
    std::vector<ValVariant> Args;
    auto ExecRes = Executor.invoke(IncrementFunc, Args, IncrParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 2u);
  }
  
  // Test get_counter() = 2
  {
    std::vector<ValVariant> Args;
    auto ExecRes = Executor.invoke(GetCounterFunc, Args, GetParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 2u);
  }
}

#endif // WASMEDGE_BUILD_IR_JIT

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
