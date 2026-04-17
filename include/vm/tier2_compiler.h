// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/vm/tier2_compiler.h - Tier-2 LLVM recompiler -------------===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Tier-2 compiler: takes a batch of hot function indices and the parent
/// AST::Module, synthesizes a mini AST::Module, feeds it through
/// WasmEdge::LLVM::Compiler to get an llvm::Module, patches in ABI thunks,
/// and ORC-JITs it. Returns (funcIdx, tier1-ABI entry pointer) pairs.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "common/errcode.h"
#include "common/span.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace WasmEdge {
namespace AST {
class Module;
} // namespace AST
namespace VM {

/// Tier-2 compiler. One instance is owned by the background Tier2Manager
/// worker. Each compileBatch() call produces a fresh ORC LLJIT containing
/// the compiled batch.
class Tier2Compiler {
public:
  Tier2Compiler() noexcept;
  ~Tier2Compiler() noexcept;

  Tier2Compiler(const Tier2Compiler &) = delete;
  Tier2Compiler &operator=(const Tier2Compiler &) = delete;

  /// Set a shutdown flag. If non-null, compileBatch() checks it at key
  /// points and bails out early if set, avoiding long-running LLVM codegen.
  void setShutdownFlag(std::atomic<bool> *Flag) noexcept {
    ShutdownFlag_ = Flag;
  }

  /// Compile a batch of hot functions by synthesizing a mini AST::Module
  /// from \p Mod that contains only the batch bodies (non-batch defined
  /// functions get `unreachable` stubs to preserve funcIdx space), feeding
  /// it through WasmEdge::LLVM::Compiler, and ORC-JITing the result.
  ///
  /// \param BatchIdx  Module-wide function indices to promote (imports are
  ///                  not valid inputs here). First entry is the hot head.
  /// \param Mod       Full parsed AST::Module. Kept alive by the manager.
  /// \param OptLevel  LLVM optimization level (0-3).
  /// \returns A vector of (funcIdx, tier1-ABI entry pointer) pairs suitable
  ///          for atomic FuncTable swap.
  Expect<std::vector<std::pair<uint32_t, void *>>>
  compileBatch(Span<const uint32_t> BatchIdx, const AST::Module &Mod,
               unsigned OptLevel = 2);

  /// Compile an OSR (on-stack replacement) entry for the given
  /// (FuncIdx, LoopIdx) pair. The target function is rewritten so its
  /// entry point is the start of the \p LoopIdx-th outermost loop; all
  /// wasm locals (params + declared) become function parameters and are
  /// passed via the tier-1 `(JitExecEnv*, uint64_t*)` ABI just like a
  /// regular fwd_thunk. Other defined functions in the module are stubbed
  /// as tier-2 → tier-1 bridges so cross-function calls resolve
  /// correctly.
  ///
  /// \param FuncIdx   Module-wide function index containing the target loop.
  /// \param LoopIdx   Index of the outermost loop (matches the
  ///                  `CurrLoopIdx++` assignment in the IR builder).
  /// \param Mod       Full parsed AST::Module. Kept alive by the manager.
  /// \param OptLevel  LLVM optimization level (0-3).
  /// \returns The native OSR entry pointer (tier-1 ABI) on success.
  Expect<void *> compileOsrEntry(uint32_t FuncIdx, uint32_t LoopIdx,
                                  const AST::Module &Mod,
                                  unsigned OptLevel = 2);

private:
  bool isShutdown() const noexcept {
    return ShutdownFlag_ && ShutdownFlag_->load(std::memory_order_acquire);
  }

  struct Impl;
  std::unique_ptr<Impl> P;
  std::atomic<bool> *ShutdownFlag_ = nullptr;
};

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
