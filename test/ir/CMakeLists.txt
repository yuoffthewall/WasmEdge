# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2019-2024 Second State INC

if(NOT WASMEDGE_BUILD_IR_JIT)
  return()
endif()

# IR JIT basic tests
wasmedge_add_executable(wasmedgeIRTests
  ir_basic_test.cpp
)

target_include_directories(wasmedgeIRTests
  PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/test
)

target_link_libraries(wasmedgeIRTests
  PRIVATE
  wasmedgeVM
  wasmedgeIR
  wasmedgeCommon
  GTest::gtest_main
)

add_test(NAME IRTests COMMAND wasmedgeIRTests)

# IR JIT instruction lowering tests (comprehensive)
wasmedge_add_executable(wasmedgeIRInstructionTests
  ir_instruction_test.cpp
)

target_include_directories(wasmedgeIRInstructionTests
  PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/test
)

target_link_libraries(wasmedgeIRInstructionTests
  PRIVATE
  wasmedgeVM
  wasmedgeIR
  wasmedgeCommon
  GTest::gtest_main
)

add_test(NAME IRInstructionTests COMMAND wasmedgeIRInstructionTests)

# IR JIT execution correctness tests (verifies computed results)
wasmedge_add_executable(wasmedgeIRExecutionTests
  ir_execution_test.cpp
)

target_include_directories(wasmedgeIRExecutionTests
  PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/test
)

target_link_libraries(wasmedgeIRExecutionTests
  PRIVATE
  wasmedgeVM
  wasmedgeIR
  wasmedgeCommon
  GTest::gtest_main
)

add_test(NAME IRExecutionTests COMMAND wasmedgeIRExecutionTests)

# IR JIT integration tests (tests with WasmEdge runtime)
wasmedge_add_executable(wasmedgeIRIntegrationTests
  ir_integration_test.cpp
)

target_include_directories(wasmedgeIRIntegrationTests
  PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/test
)

target_link_libraries(wasmedgeIRIntegrationTests
  PRIVATE
  wasmedgeVM
  wasmedgeIR
  wasmedgeExecutor
  wasmedgeCommon
  GTest::gtest_main
)

add_test(NAME IRIntegrationTests COMMAND wasmedgeIRIntegrationTests)

# IR JIT end-to-end tests (with real .wasm files)
wasmedge_add_executable(wasmedgeIRE2ETests
  ir_e2e_test.cpp
)

target_include_directories(wasmedgeIRE2ETests
  PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/test
)

target_link_libraries(wasmedgeIRE2ETests
  PRIVATE
  wasmedgeLoader
  wasmedgeValidator
  wasmedgeExecutor
  wasmedgeVM
  wasmedgeIR
  wasmedgeCommon
  GTest::gtest_main
)

add_test(NAME IRE2ETests COMMAND wasmedgeIRE2ETests)

# IR JIT benchmark tests (integer algorithms)
wasmedge_add_executable(wasmedgeIRBenchmarkTests
  ir_benchmark_test.cpp
)

target_include_directories(wasmedgeIRBenchmarkTests
  PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/test
)

target_link_libraries(wasmedgeIRBenchmarkTests
  PRIVATE
  wasmedgeLoader
  wasmedgeValidator
  wasmedgeExecutor
  wasmedgeVM
  wasmedgeIR
  wasmedgeCommon
  GTest::gtest_main
)

add_test(NAME IRBenchmarkTests COMMAND wasmedgeIRBenchmarkTests)
