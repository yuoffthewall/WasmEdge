// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/vm/tier2_compiler.h - Tier-2 LLVM recompiler -------------===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Tier-2 compiler: takes a dstogov/ir graph (ir_ctx*) preserved from tier-1
/// JIT compilation, emits LLVM IR via ir_emit_llvm(), optimizes with LLVM,
/// and produces native code via ORC LLJIT.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "common/errcode.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct _ir_ctx;
typedef struct _ir_ctx ir_ctx;

namespace WasmEdge::VM {

/// Result of tier-2 compilation.
struct Tier2CompileResult {
  void *NativeFunc = nullptr; // Pointer to LLVM-compiled native code
  size_t CodeSize = 0;        // Approximate code size
};

/// Tier-2 compiler: ir_ctx* → LLVM IR → optimized native code.
///
/// Holds an ORC LLJIT instance that persists for the lifetime of the compiler,
/// accumulating compiled functions. Each compile() call adds a new function
/// to the JIT and returns a callable pointer.
class Tier2Compiler {
public:
  Tier2Compiler() noexcept;
  ~Tier2Compiler() noexcept;

  Tier2Compiler(const Tier2Compiler &) = delete;
  Tier2Compiler &operator=(const Tier2Compiler &) = delete;

  /// Compile a single function from its preserved ir_ctx* graph.
  /// \param Ctx       The ir_ctx* from tier-1 (must have been through ir_build).
  /// \param FuncName  Name for the compiled function (e.g. "wasm_tier2_042").
  /// \param OptLevel  LLVM optimization level (0-3), default 2.
  /// \returns Native function pointer on success.
  Expect<Tier2CompileResult> compile(ir_ctx *Ctx, const std::string &FuncName,
                                     unsigned OptLevel = 2);

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
