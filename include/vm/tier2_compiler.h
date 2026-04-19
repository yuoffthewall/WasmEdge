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

/// Result of a unified tier-2 compile.
///   - FwdThunks: (funcIdx, native ptr) pairs for FuncTable swap. Populated
///     when LoopEntries was empty (pure function-entry batch).
///   - OsrEntries: (loopIdx, native ptr) pairs for the OsrEntryTable slot
///     at `RootFuncIdx * OSR_MAX_LOOPS_PER_FUNC + loopIdx`. Populated when
///     LoopEntries was non-empty.
struct Tier2CompileResult {
  std::vector<std::pair<uint32_t, void *>> FwdThunks;
  std::vector<std::pair<uint32_t, void *>> OsrEntries;
};

/// Tier-2 compiler. One instance is owned by the background Tier2Manager
/// worker. Each compile call produces a fresh ORC LLJIT containing
/// the compiled batch.
class Tier2Compiler {
public:
  Tier2Compiler() noexcept;
  ~Tier2Compiler() noexcept;

  Tier2Compiler(const Tier2Compiler &) = delete;
  Tier2Compiler &operator=(const Tier2Compiler &) = delete;

  /// Set a shutdown flag. If non-null, compile paths check it at key
  /// points and bail out early if set, avoiding long-running LLVM codegen.
  void setShutdownFlag(std::atomic<bool> *Flag) noexcept {
    ShutdownFlag_ = Flag;
  }

  /// Unified tier-2 compile entry. Synthesizes a mini AST::Module that
  /// keeps real bodies for \p Batch members, optionally appends one OSR
  /// function slot per `LoopEntries` index (whose body starts at the loop
  /// header inside `RootFuncIdx`), stubs everything else, and ORC-JITs
  /// the result.
  ///
  /// \param RootFuncIdx Anchor function (used for OSR slot synthesis and
  ///                    dump/log file naming).
  /// \param Batch       Manager-resolved batch members (real bodies kept
  ///                    so LLVM can inline). First entry is the hot head.
  /// \param LoopEntries OSR loop indices to synthesize entry slots for.
  ///                    Empty for a pure function-entry tier-2 batch.
  /// \param Mod         Full parsed AST::Module. Kept alive by the manager.
  /// \param OptLevel    LLVM optimization level (0-3).
  Expect<Tier2CompileResult>
  compileRequest(uint32_t RootFuncIdx, Span<const uint32_t> Batch,
                 Span<const uint32_t> LoopEntries, const AST::Module &Mod,
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
