# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2019-2024 Second State INC

# `libwasmedge_engine.so` — sightglass-cli engine plugin.
#
# Built only when WASMEDGE_BUILD_IR_JIT is on, since the point of this shim
# is to drive the IR-JIT path (and compare it against LLVM JIT / Interpreter)
# under sightglass's measurement harness.
#
# Build:    make wasmedge_engine
# Output:   build/tools/sightglass_engine/libwasmedge_engine.so
# Use:      sightglass-cli benchmark --engine $(pwd)/build/.../libwasmedge_engine.so

if(NOT WASMEDGE_BUILD_IR_JIT)
  return()
endif()

wasmedge_add_library(wasmedge_engine SHARED
  engine.cpp
)

set_target_properties(wasmedge_engine PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(wasmedge_engine
  PRIVATE
  ${PROJECT_SOURCE_DIR}/include
)

# Without this flag, the runtime/instance/function.h struct layout
# diverges from libwasmedgeVM/Executor (the IR-JIT variant of the
# FunctionInstance::Data std::variant disappears), producing ODR
# violations at link time.
target_compile_definitions(wasmedge_engine
  PRIVATE
  WASMEDGE_BUILD_IR_JIT
)

target_link_libraries(wasmedge_engine
  PRIVATE
  wasmedgeLoader
  wasmedgeValidator
  wasmedgeExecutor
  wasmedgeVM
  wasmedgeIR
  wasmedgeHostModuleWasi
  wasmedgeCommon
)

if(WASMEDGE_USE_LLVM)
  target_compile_definitions(wasmedge_engine PRIVATE WASMEDGE_USE_LLVM)
  target_link_libraries(wasmedge_engine PRIVATE wasmedgeLLVM)
endif()
