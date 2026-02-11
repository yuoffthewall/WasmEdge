// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- test/ir/ir_benchmark_test.cpp - IR JIT Benchmark Tests ------------===//
//
// Benchmark tests for IR JIT execution using integer-only algorithms.
// Based on popular Sightglass benchmarks (fibonacci, ackermann, etc.)
//
//===----------------------------------------------------------------------===//

#include "common/configure.h"
#include "executor/executor.h"
#include "loader/loader.h"
#include "validator/validator.h"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>

namespace {

class IRBenchmarkTest : public ::testing::Test {
protected:
  void SetUp() override {
    Conf.getCompilerConfigure().setOptimizationLevel(
        WasmEdge::CompilerConfigure::OptimizationLevel::O0);
  }

  std::filesystem::path getTestDataPath() {
    return std::filesystem::path(__FILE__).parent_path() / "testdata";
  }

  WasmEdge::Configure Conf;
};

// Helper to measure execution time
template <typename Func>
double measureTime(Func &&f, int iterations = 1) {
  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    f();
  }
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;
  return elapsed.count() / iterations;
}

// ============================================================================
// Correctness Tests
// ============================================================================

TEST_F(IRBenchmarkTest, FibonacciRecursive_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("fib_recursive");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Test known Fibonacci values
  struct TestCase {
    uint32_t n;
    uint32_t expected;
  };
  std::vector<TestCase> cases = {
      {0, 0},   {1, 1},   {2, 1},   {3, 2},   {4, 3},
      {5, 5},   {6, 8},   {7, 13},  {8, 21},  {9, 34},
      {10, 55}
  };

  for (const auto &tc : cases) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(tc.n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value()) << "Failed for n=" << tc.n;
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), tc.expected)
        << "fib_recursive(" << tc.n << ") should be " << tc.expected;
  }
}

TEST_F(IRBenchmarkTest, FibonacciIterative_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("fib_iterative");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Test known Fibonacci values
  struct TestCase {
    uint32_t n;
    uint32_t expected;
  };
  std::vector<TestCase> cases = {
      {0, 0},   {1, 1},   {2, 1},    {3, 2},     {4, 3},
      {5, 5},   {6, 8},   {7, 13},   {8, 21},    {9, 34},
      {10, 55}, {15, 610}, {20, 6765}, {25, 75025}
  };

  for (const auto &tc : cases) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(tc.n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value()) << "Failed for n=" << tc.n;
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), tc.expected)
        << "fib(" << tc.n << ") should be " << tc.expected;
  }
}

TEST_F(IRBenchmarkTest, Ackermann_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("ackermann");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Ackermann function values
  struct TestCase {
    uint32_t m, n, expected;
  };
  std::vector<TestCase> cases = {
      {0, 0, 1},   {0, 1, 2},   {0, 5, 6},
      {1, 0, 2},   {1, 1, 3},   {1, 5, 7},
      {2, 0, 3},   {2, 1, 5},   {2, 2, 7},
      {3, 0, 5},   {3, 1, 13},  {3, 2, 29}
  };

  for (const auto &tc : cases) {
    std::vector<WasmEdge::ValVariant> Args = {
        WasmEdge::ValVariant(tc.m), WasmEdge::ValVariant(tc.n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value()) << "Failed for ack(" << tc.m << ", " << tc.n << ")";
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), tc.expected)
        << "ack(" << tc.m << ", " << tc.n << ") should be " << tc.expected;
  }
}

TEST_F(IRBenchmarkTest, SumToN_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("sum_to_n");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Test: sum(1..n) = n*(n+1)/2
  std::vector<uint32_t> testValues = {0, 1, 5, 10, 100, 1000, 10000};
  for (uint32_t n : testValues) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    uint32_t expected = n * (n + 1) / 2;
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), expected)
        << "sum_to_n(" << n << ") should be " << expected;
  }
}

TEST_F(IRBenchmarkTest, GCD_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("gcd");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  struct TestCase {
    uint32_t a, b, expected;
  };
  std::vector<TestCase> cases = {
      {48, 18, 6},   {100, 25, 25}, {17, 13, 1}, {270, 192, 6},
      {1071, 462, 21}, {0, 5, 5},     {5, 0, 5}
  };

  for (const auto &tc : cases) {
    std::vector<WasmEdge::ValVariant> Args = {
        WasmEdge::ValVariant(tc.a), WasmEdge::ValVariant(tc.b)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), tc.expected)
        << "gcd(" << tc.a << ", " << tc.b << ") should be " << tc.expected;
  }
}

TEST_F(IRBenchmarkTest, TestIsPrimeWrapper_Correctness) {
  // This tests JIT-to-JIT function calls by using a simple wrapper
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("test_is_prime");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Test known primes via wrapper
  std::vector<uint32_t> primes = {2, 3, 5, 7, 11};
  for (uint32_t p : primes) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(p)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 1u) 
        << "test_is_prime(" << p << ") should be 1 (prime)";
  }

  // Test non-primes via wrapper
  std::vector<uint32_t> nonPrimes = {0, 1, 4, 6, 8, 9, 10};
  for (uint32_t n : nonPrimes) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 0u)
        << "test_is_prime(" << n << ") should be 0 (not prime)";
  }
}

TEST_F(IRBenchmarkTest, IsPrime_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("is_prime");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Known primes
  std::vector<uint32_t> primes = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 97, 101};
  for (uint32_t p : primes) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(p)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 1u) << p << " should be prime";
  }

  // Non-primes
  std::vector<uint32_t> nonPrimes = {0, 1, 4, 6, 8, 9, 10, 15, 21, 100};
  for (uint32_t n : nonPrimes) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 0u)
        << n << " should not be prime";
  }
}

TEST_F(IRBenchmarkTest, CountPrimes_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("count_primes");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  struct TestCase {
    uint32_t n, expected;
  };
  // π(n) - prime counting function
  std::vector<TestCase> cases = {
      {10, 4},   // 2, 3, 5, 7
      {20, 8},   // 2, 3, 5, 7, 11, 13, 17, 19
      {100, 25}, // Known: π(100) = 25
  };

  for (const auto &tc : cases) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(tc.n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), tc.expected)
        << "count_primes(" << tc.n << ") should be " << tc.expected;
  }
}

// ============================================================================
// Performance Benchmarks
// ============================================================================

TEST_F(IRBenchmarkTest, Benchmark_FibonacciIterative) {
  // Disabled by default - run with --gtest_also_run_disabled_tests
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("fib_iterative");
  ASSERT_NE(FuncInst, nullptr);
  
  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  const int ITERATIONS = 100000;
  std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(uint32_t(35))};

  double avgTime = measureTime(
      [&]() { Executor.invoke(FuncInst, Args, ParamTypes); }, ITERATIONS);

  std::cout << "\n=== Fibonacci Iterative Benchmark ===" << std::endl;
  std::cout << "  fib(35) x " << ITERATIONS << " iterations" << std::endl;
  std::cout << "  JIT: " << FuncInst->isIRJitFunction() << std::endl;
  std::cout << "  Avg time per call: " << std::fixed << std::setprecision(3)
            << avgTime << " ms" << std::endl;
  std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
            << (1000.0 / avgTime) << " calls/sec" << std::endl;
}

TEST_F(IRBenchmarkTest, Benchmark_CountPrimes) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("count_primes");
  ASSERT_NE(FuncInst, nullptr);

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  const int ITERATIONS = 1000;
  std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(uint32_t(1000))};

  double avgTime = measureTime(
      [&]() { Executor.invoke(FuncInst, Args, ParamTypes); }, ITERATIONS);

  std::cout << "\n=== Count Primes Benchmark ===" << std::endl;
  std::cout << "  count_primes(1000) x " << ITERATIONS << " iterations" << std::endl;
  std::cout << "  JIT: " << FuncInst->isIRJitFunction() << std::endl;
  std::cout << "  Avg time per call: " << std::fixed << std::setprecision(3)
            << avgTime << " ms" << std::endl;
  std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
            << (1000.0 / avgTime) << " calls/sec" << std::endl;
}

// ============================================================================
// Quicksort Tests - Memory operations + recursion
// ============================================================================

TEST_F(IRBenchmarkTest, Quicksort_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  
  // Get function pointers
  auto *QuicksortFunc = ModInst->findFuncExports("quicksort");
  auto *IsSortedFunc = ModInst->findFuncExports("is_sorted");
  ASSERT_NE(QuicksortFunc, nullptr);
  ASSERT_NE(IsSortedFunc, nullptr);
  EXPECT_TRUE(QuicksortFunc->isIRJitFunction()) << "quicksort should be JIT compiled";
  EXPECT_TRUE(IsSortedFunc->isIRJitFunction()) << "is_sorted should be JIT compiled";

  // Get memory instance
  auto MemInsts = ModInst->getMemoryInstances();
  ASSERT_FALSE(MemInsts.empty());
  auto *MemInst = MemInsts[0];
  ASSERT_NE(MemInst, nullptr);

  auto QuicksortParams = QuicksortFunc->getFuncType().getParamTypes();
  auto IsSortedParams = IsSortedFunc->getFuncType().getParamTypes();

  // Test case 1: Sort a small array [5, 2, 8, 1, 9]
  {
    const uint32_t BASE = 0;
    const uint32_t LEN = 5;
    int32_t testData[] = {5, 2, 8, 1, 9};
    
    // Write test data to memory
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      ASSERT_NE(ptr, nullptr);
      *reinterpret_cast<int32_t *>(ptr) = testData[i];
    }

    // Call quicksort(base=0, low=0, high=4)
    std::vector<WasmEdge::ValVariant> SortArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(0)),
        WasmEdge::ValVariant(uint32_t(LEN - 1))};
    auto SortRes = Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
    ASSERT_TRUE(SortRes.has_value()) << "quicksort failed";

    // Verify sorted with is_sorted
    std::vector<WasmEdge::ValVariant> CheckArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(LEN))};
    auto CheckRes = Executor.invoke(IsSortedFunc, CheckArgs, IsSortedParams);
    ASSERT_TRUE(CheckRes.has_value());
    EXPECT_EQ((*CheckRes)[0].first.get<uint32_t>(), 1u) << "Array should be sorted";

    // Also verify manually
    int32_t expected[] = {1, 2, 5, 8, 9};
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      int32_t val = *reinterpret_cast<int32_t *>(ptr);
      EXPECT_EQ(val, expected[i]) << "Element at index " << i << " incorrect";
    }
  }

  // Test case 2: Already sorted array
  {
    const uint32_t BASE = 100;
    const uint32_t LEN = 4;
    int32_t testData[] = {1, 2, 3, 4};
    
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      *reinterpret_cast<int32_t *>(ptr) = testData[i];
    }

    std::vector<WasmEdge::ValVariant> SortArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(0)),
        WasmEdge::ValVariant(uint32_t(LEN - 1))};
    auto SortRes = Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
    ASSERT_TRUE(SortRes.has_value());

    std::vector<WasmEdge::ValVariant> CheckArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(LEN))};
    auto CheckRes = Executor.invoke(IsSortedFunc, CheckArgs, IsSortedParams);
    ASSERT_TRUE(CheckRes.has_value());
    EXPECT_EQ((*CheckRes)[0].first.get<uint32_t>(), 1u) << "Already sorted array should remain sorted";
  }

  // Test case 3: Reverse sorted array
  {
    const uint32_t BASE = 200;
    const uint32_t LEN = 6;
    int32_t testData[] = {6, 5, 4, 3, 2, 1};
    
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      *reinterpret_cast<int32_t *>(ptr) = testData[i];
    }

    std::vector<WasmEdge::ValVariant> SortArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(0)),
        WasmEdge::ValVariant(uint32_t(LEN - 1))};
    auto SortRes = Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
    ASSERT_TRUE(SortRes.has_value());

    std::vector<WasmEdge::ValVariant> CheckArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(LEN))};
    auto CheckRes = Executor.invoke(IsSortedFunc, CheckArgs, IsSortedParams);
    ASSERT_TRUE(CheckRes.has_value());
    EXPECT_EQ((*CheckRes)[0].first.get<uint32_t>(), 1u) << "Reverse array should be sorted";
  }

  // Test case 4: Single element (edge case)
  {
    const uint32_t BASE = 300;
    uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE);
    *reinterpret_cast<int32_t *>(ptr) = 42;

    std::vector<WasmEdge::ValVariant> SortArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(0)),
        WasmEdge::ValVariant(uint32_t(0))};
    auto SortRes = Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
    ASSERT_TRUE(SortRes.has_value());

    std::vector<WasmEdge::ValVariant> CheckArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(1))};
    auto CheckRes = Executor.invoke(IsSortedFunc, CheckArgs, IsSortedParams);
    ASSERT_TRUE(CheckRes.has_value());
    EXPECT_EQ((*CheckRes)[0].first.get<uint32_t>(), 1u) << "Single element should be sorted";
  }

  // Test case 5: Array with duplicates
  {
    const uint32_t BASE = 400;
    const uint32_t LEN = 7;
    int32_t testData[] = {3, 1, 4, 1, 5, 9, 2};
    
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      *reinterpret_cast<int32_t *>(ptr) = testData[i];
    }

    std::vector<WasmEdge::ValVariant> SortArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(0)),
        WasmEdge::ValVariant(uint32_t(LEN - 1))};
    auto SortRes = Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
    ASSERT_TRUE(SortRes.has_value());

    std::vector<WasmEdge::ValVariant> CheckArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(LEN))};
    auto CheckRes = Executor.invoke(IsSortedFunc, CheckArgs, IsSortedParams);
    ASSERT_TRUE(CheckRes.has_value());
    EXPECT_EQ((*CheckRes)[0].first.get<uint32_t>(), 1u) << "Array with duplicates should be sorted";
  }
}

TEST_F(IRBenchmarkTest, Benchmark_Quicksort) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *QuicksortFunc = ModInst->findFuncExports("quicksort");
  ASSERT_NE(QuicksortFunc, nullptr);

  auto MemInsts = ModInst->getMemoryInstances();
  ASSERT_FALSE(MemInsts.empty());
  auto *MemInst = MemInsts[0];

  auto QuicksortParams = QuicksortFunc->getFuncType().getParamTypes();

  const uint32_t BASE = 0;
  const uint32_t LEN = 100;
  const int ITERATIONS = 1000;

  // Generate reverse-sorted array (worst case for naive quicksort, but our Lomuto is decent)
  auto initArray = [&]() {
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      *reinterpret_cast<int32_t *>(ptr) = static_cast<int32_t>(LEN - i);
    }
  };

  std::vector<WasmEdge::ValVariant> SortArgs = {
      WasmEdge::ValVariant(uint32_t(BASE)),
      WasmEdge::ValVariant(uint32_t(0)),
      WasmEdge::ValVariant(uint32_t(LEN - 1))};

  double avgTime = measureTime(
      [&]() {
        initArray();  // Reset array before each sort
        Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
      },
      ITERATIONS);

  std::cout << "\n=== Quicksort Benchmark ===" << std::endl;
  std::cout << "  quicksort(" << LEN << " elements) x " << ITERATIONS << " iterations" << std::endl;
  std::cout << "  JIT: " << QuicksortFunc->isIRJitFunction() << std::endl;
  std::cout << "  Avg time per call: " << std::fixed << std::setprecision(3)
            << avgTime << " ms" << std::endl;
  std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
            << (1000.0 / avgTime) << " sorts/sec" << std::endl;
}

} // namespace
