// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/vm/tier2_compiler.h - Tier-2 LLVM recompiler -------------===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Tier-2 compiler: takes pre-emitted LLVM IR text (generated from the
/// dstogov/ir graph before tier-1 compilation), optimizes with LLVM,
/// and produces native code via ORC LLJIT.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "common/errcode.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace WasmEdge::VM {

/// Result of tier-2 compilation.
struct Tier2CompileResult {
  void *NativeFunc = nullptr; // Pointer to LLVM-compiled native code
  size_t CodeSize = 0;        // Approximate code size
};

/// Tier-2 compiler: serialized IR text → LLVM IR → optimized native code.
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

  /// Set a shutdown flag. If non-null, compile() checks it at key points
  /// and bails out early if set, avoiding long-running LLVM codegen calls.
  void setShutdownFlag(std::atomic<bool> *Flag) noexcept { ShutdownFlag_ = Flag; }

  /// Compile a single function from its serialized dstogov/ir text.
  /// The text is loaded into a fresh ir_ctx, converted to LLVM IR via
  /// ir_emit_llvm, then optimized and compiled to native code.
  /// \param IRText    Serialized IR text (from ir_save before tier-1 compile).
  /// \param FuncName  Name for the compiled function (e.g. "wasm_tier2_042").
  /// \param RetType   ir_type of return value (from tier-1 ir_ctx->ret_type).
  /// \param OptLevel  LLVM optimization level (0-3), default 2.
  /// \returns Native function pointer on success.
  Expect<Tier2CompileResult> compile(const std::string &IRText,
                                     const std::string &FuncName,
                                     uint8_t RetType,
                                     unsigned OptLevel = 2);

private:
  bool isShutdown() const noexcept {
    return ShutdownFlag_ && ShutdownFlag_->load(std::memory_order_acquire);
  }

  struct Impl;
  std::unique_ptr<Impl> P;
  std::atomic<bool> *ShutdownFlag_ = nullptr;
};

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
