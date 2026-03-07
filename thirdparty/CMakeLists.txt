# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2019-2024 Second State INC

if(WASMEDGE_USE_LLVM)
  add_subdirectory(blake3)
endif()

# IR JIT dependency: dstogov/ir
# We treat it as an external project built with its own Makefile.
# CMake consumes the prebuilt static library via an IMPORTED target.
if(WASMEDGE_BUILD_IR_JIT)
  set(WASMEDGE_IR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ir")
  set(WASMEDGE_IR_LIB "${WASMEDGE_IR_SOURCE_DIR}/libir.a")

  # Automatically build dstogov/ir when IR JIT is enabled so that a fresh
  # clone can be configured and built on Linux/macOS without manual steps.
  #
  # On Linux we must compile libir.a as PIC so it can be linked into
  # libwasmedge.so; do that by injecting -fPIC via the environment.
  set(_ir_env)
  if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    list(APPEND _ir_env "CFLAGS=-fPIC" "BUILD_CFLAGS=-fPIC")
  endif()

  # Select the correct IR JIT target for the host CPU.
  # dstogov/ir defaults to x86_64; on Apple Silicon / other AArch64
  # platforms we must explicitly request the aarch64 backend.
  # Use debug build for IR_DEBUG assertions and better GDB support.
  set(_ir_make_args "BUILD=debug")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    list(APPEND _ir_make_args "TARGET=aarch64")
  endif()
  
  if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    # Provide homebrew include paths so ir_disasm.c can find capstone.h
    list(APPEND _ir_make_args "CFLAGS=-I/usr/local/include -I/opt/homebrew/include")
  endif()

  add_custom_command(
    OUTPUT "${WASMEDGE_IR_LIB}"
    COMMAND ${CMAKE_COMMAND} -E env ${_ir_env} ${CMAKE_MAKE_PROGRAM} ${_ir_make_args} libir.a
    WORKING_DIRECTORY "${WASMEDGE_IR_SOURCE_DIR}"
    COMMENT "Building dstogov/ir (libir.a) for IR JIT"
    VERBATIM
  )

  add_custom_target(wasmedgeIRBuild ALL DEPENDS "${WASMEDGE_IR_LIB}")

  add_library(wasmedgeIR STATIC IMPORTED GLOBAL)
  add_dependencies(wasmedgeIR wasmedgeIRBuild)
  set_target_properties(wasmedgeIR PROPERTIES
    IMPORTED_LOCATION "${WASMEDGE_IR_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${WASMEDGE_IR_SOURCE_DIR}"
  )
endif()
