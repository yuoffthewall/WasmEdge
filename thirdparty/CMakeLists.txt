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

  if(NOT EXISTS "${WASMEDGE_IR_LIB}")
    message(FATAL_ERROR
      "IR JIT is enabled (WASMEDGE_BUILD_IR_JIT=ON), but libir.a was not found.\n"
      "Expected: ${WASMEDGE_IR_LIB}\n"
      "Please build dstogov/ir first, e.g.:\n"
      "  cd thirdparty/ir\n"
      "  make libir.a            # x86_64\n"
      "  make TARGET=aarch64 libir.a  # Apple Silicon / arm64\n")
  endif()

  add_library(wasmedgeIR STATIC IMPORTED GLOBAL)
  set_target_properties(wasmedgeIR PROPERTIES
    IMPORTED_LOCATION "${WASMEDGE_IR_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${WASMEDGE_IR_SOURCE_DIR}"
  )
endif()
