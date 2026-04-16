// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/vm/tier2_manager.h - Tier-2 recompilation manager --------===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Manages tier-2 (LLVM) recompilation of hot functions. Receives tier-up
/// requests from JIT function prologues, compiles on a background thread,
/// and installs the results by swapping FuncTable pointers.
///
//===----------------------------------------------------------------------===//
#pragma once

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

namespace WasmEdge {
namespace AST {
class Module;
} // namespace AST
namespace VM {

class Tier2Manager {
public:
  Tier2Manager() noexcept;
  ~Tier2Manager() noexcept;

  Tier2Manager(const Tier2Manager &) = delete;
  Tier2Manager &operator=(const Tier2Manager &) = delete;

  /// Signal the worker to stop (non-blocking). Used by atexit handler
  /// before _exit(0) to give the worker a chance to check Shutdown_.
  void shutdown() noexcept;

  /// Enqueue a function for tier-2 recompilation.
  /// Called from jit_tier_up_notify (JIT code context).
  /// \param FuncIdx      The function that tripped the tier-up threshold.
  /// \param Mod          Shared pointer to the full parsed AST::Module.
  /// \param FuncTable    Shared pointer to the FuncTable array (for live
  ///                     code swap).
  /// \param CallCounters Read-only pointer to the tier-1 per-function call
  ///                     counter array (from JitExecEnv). Used by the
  ///                     walk-up phase to score ancestor warmth. May be
  ///                     null; walk-up is skipped in that case.
  void enqueue(uint32_t FuncIdx,
               std::shared_ptr<const AST::Module> Mod,
               std::shared_ptr<void *[]> FuncTable,
               const uint32_t *CallCounters) noexcept;

  /// Number of functions successfully recompiled to tier-2.
  uint32_t tier2Count() const noexcept {
    return Tier2Count_.load(std::memory_order_relaxed);
  }

private:
  /// Static per-module forward+reverse call graph, built once and cached.
  struct ModuleCG {
    uint32_t ImportFuncNum = 0;
    /// Indexed by defined-function index (funcIdx - ImportFuncNum).
    /// Each entry is the list of direct Call targets (module-wide funcIdx).
    std::vector<std::vector<uint32_t>> Callees;
    /// Reverse edges: callers[defined_idx] = list of module-wide funcIdx
    /// that directly Call this function.
    std::vector<std::vector<uint32_t>> Callers;
  };

  /// A batch request carries the fully-resolved set of funcIdxs to compile
  /// together. The worker does no further expansion.
  struct Request {
    uint32_t HotFuncIdx;        // function that tripped the threshold
    uint32_t RootFuncIdx;       // chosen batch anchor (may equal HotFuncIdx)
    uint32_t WalkupHops;        // telemetry
    std::vector<uint32_t> Batch;
    std::shared_ptr<const AST::Module> Mod;
    std::shared_ptr<void *[]> FuncTable;
  };

  void workerLoop();

  /// Build (or return cached) call graph for \p Mod. Caller must hold Mu_.
  const ModuleCG &getOrBuildCGLocked(const AST::Module &Mod) noexcept;

  /// Walk up the call graph from HotFuncIdx, picking the hottest warm
  /// ancestor at each step. Returns (root, hops). Caller must hold Mu_.
  std::pair<uint32_t, uint32_t>
  walkUpRootLocked(const AST::Module &Mod, const ModuleCG &CG,
                   uint32_t HotFuncIdx, uintptr_t FTKey,
                   const uint32_t *CallCounters) noexcept;

  /// BFS down from Root collecting scalar-promotable descendants not yet
  /// in Seen_, bounded by BfsMaxDepth_ and MaxBatchSize_. Callees with
  /// zero call count are excluded (never actually called at runtime).
  /// Ensures HotFuncIdx appears in the result. Caller must hold Mu_.
  std::vector<uint32_t>
  bfsDownBatchLocked(const AST::Module &Mod, const ModuleCG &CG,
                     uint32_t Root, uint32_t HotFuncIdx,
                     uintptr_t FTKey,
                     const uint32_t *CallCounters) noexcept;

  std::thread Worker_;
  std::mutex Mu_;
  std::condition_variable CV_;
  std::queue<Request> Queue_;
  std::set<std::pair<uintptr_t, uint32_t>> Seen_;
  std::unordered_map<const AST::Module *, ModuleCG> CGCache_;
  std::atomic<bool> Shutdown_{false};
  std::atomic<bool> WorkerDone_{false};
  std::atomic<uint32_t> Tier2Count_{0};

  // Threshold loaded from env var; batching geometry is fixed.
  uint32_t Tier2Threshold_ = 1000;      // WASMEDGE_TIER2_THRESHOLD
  // Warm floor = Threshold/256 ≈ 39 calls. Picked empirically from a
  // WARM_DIVISOR sweep on the loss cluster (divisor 2 was ~30x too strict;
  // 256 reliably batches {root,hot,siblings}). See
  // notes/benchmarking/tier2_v2_vs_llvm_jit_benchmark.md.
  static constexpr uint32_t WarmDivisor_ = 256;
  static constexpr uint32_t WalkupMaxDepth_ = 1;
  static constexpr uint32_t BfsMaxDepth_ = 2;
  static constexpr uint32_t MaxBatchSize_ = 12;

  // Telemetry aggregates (dumped at shutdown).
  uint64_t StatBatches_ = 0;
  uint64_t StatSingletons_ = 0;
  uint64_t StatWalkupHits_ = 0;
  uint64_t StatBatchMemberSum_ = 0;
};

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
