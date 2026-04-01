# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2019-2024 Second State INC

set(WASMEDGE_VM_SOURCES
  vm.cpp
)

if(WASMEDGE_BUILD_IR_JIT)
  list(APPEND WASMEDGE_VM_SOURCES
    ir_builder.cpp
    ir_jit_engine.cpp
    ir_jit_reg_invoke.cpp
    ir_jit_reg_invoke_autogen.cpp
  )
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
  target_compile_definitions(wasmedgeVM
    PUBLIC
    -DWASMEDGE_USE_LLVM
  )
  target_link_libraries(wasmedgeVM
    PUBLIC
    wasmedgeLLVM
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
