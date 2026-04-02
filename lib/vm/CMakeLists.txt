# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2019-2024 Second State INC

set(WASMEDGE_VM_SOURCES
  vm.cpp
)

if(WASMEDGE_BUILD_IR_JIT)
  list(APPEND WASMEDGE_VM_SOURCES
    ir_builder.cpp
    ir_jit_engine.cpp
  )
  if(WASMEDGE_USE_LLVM)
    list(APPEND WASMEDGE_VM_SOURCES
      tier2_compiler.cpp
      tier2_manager.cpp
    )
  endif()
endif()

wasmedge_add_library(wasmedgeVM
  ${WASMEDGE_VM_SOURCES}
)

target_link_libraries(wasmedgeVM
  PUBLIC
  wasmedgeCommon
  wasmedgeSystem
  wasmedgePlugin
  wasmedgeLoader
  wasmedgeValidator
  wasmedgeExecutor
  wasmedgeHostModuleWasi
)

if(WASMEDGE_USE_LLVM)
  find_package(LLVM REQUIRED HINTS "${LLVM_CMAKE_PATH}" "${LLVM_DIR}")
  target_compile_definitions(wasmedgeVM
    PUBLIC
    -DWASMEDGE_USE_LLVM
  )
  target_link_libraries(wasmedgeVM
    PUBLIC
    wasmedgeLLVM
  )
  target_include_directories(wasmedgeVM
    SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIR}
  )
endif()

if(WASMEDGE_BUILD_IR_JIT)
  target_compile_definitions(wasmedgeVM
    PUBLIC
    -DWASMEDGE_BUILD_IR_JIT
  )
  target_link_libraries(wasmedgeVM
    PUBLIC
    wasmedgeIR
  )
endif()
