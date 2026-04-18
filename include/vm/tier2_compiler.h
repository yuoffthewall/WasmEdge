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
class FunctionType;
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

/// Entry thunk request: one LLVM-ABI wrapper to emit for an IR-JIT
/// function. Populated by the instantiation path once all IR-JIT native
/// pointers are available.
struct IRJitEntryThunkReq {
  /// Module-wide function index (for naming / diagnostics only — not
  /// baked into the generated code).
  uint32_t FuncIdx;
  /// Tier-1 native code pointer (`ret fastcall(JitExecEnv*, uint64_t*)`)
  /// to forward calls to. Embedded in the thunk as a constant.
  void *NativeFunc;
  /// Wasm function type; determines the LLVM-native thunk signature.
  const AST::FunctionType *FuncType;
};

/// Batch-build LLVM-ABI entry thunks for a list of IR-JIT functions.
///
/// Each thunk has the signature `ret (ExecCtx*, typed_params...)` — the
/// same shape `compileIndirectCallOp`'s NotNullBB expects — and forwards
/// to the underlying tier-1 native pointer through an inline `movq
/// %fs:OFFSET, $0` load of `wasmedge_tier2_jit_env_tls`. Produces one
/// ORC LLJIT that keeps the thunks alive for the Executor's lifetime.
///
/// Returns a vector of (FuncIdx, thunk_native_ptr). Requests whose
/// FuncType has non-scalar params or multi-return are skipped (caller
/// gets no entry for that FuncIdx). The returned LLJIT handle is
/// opaque; callers must retain it for the process lifetime.
struct IRJitEntryThunksResult {
  std::vector<std::pair<uint32_t, void *>> Thunks;
  /// Opaque handle that owns the ORC LLJIT; destroying it releases the
  /// thunk code. Callers must keep it alive for as long as any thunk
  /// address is reachable.
  std::shared_ptr<void> Keepalive;
};

Expect<IRJitEntryThunksResult>
buildIRJitEntryThunks(Span<const IRJitEntryThunkReq> Reqs);

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
