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

  /// Enqueue an OSR (on-stack replacement) compilation request for a
  /// (FuncIdx, LoopIdx) pair. Called from jit_osr_notify when a tier-1
  /// loop's back-edge counter saturates. The worker compiles an OSR
  /// entry point and writes its native address into
  /// `OsrEntryTable[FuncIdx * OSR_MAX_LOOPS_PER_FUNC + LoopIdx]`.
  void enqueueOsr(uint32_t FuncIdx, uint32_t LoopIdx,
                  std::shared_ptr<const AST::Module> Mod,
                  std::shared_ptr<void *[]> FuncTable,
                  std::shared_ptr<void *[]> OsrEntryTable) noexcept;

  /// Number of functions successfully recompiled to tier-2.
  uint32_t tier2Count() const noexcept {
    return Tier2Count_.load(std::memory_order_relaxed);
  }

private:
  /// Static per-module forward+reverse call graph, built once and cached.
  struct ModuleCG {
    uint32_t ImportFuncNum = 0;
    /// Forward edges: callees[defined_idx] = {(targetFuncIdx, staticFreq)}.
    /// `staticFreq` counts how many times the caller body statically calls
    /// the target — a structural "how hot would this be if called once"
    /// signal that complements the runtime counter. BFS uses it to include
    /// siblings whose runtime count is still cold at the moment of the
    /// first tier-up (e.g. ed25519: f8 statically calls f19×805 + f6×60 +
    /// f10×8, but when f19 trips threshold=10 the rest are still at 0).
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> Callees;
    /// Reverse edges: callers[defined_idx] = list of module-wide funcIdx
    /// that directly Call this function (dedup, no frequency).
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

  /// OSR-specific request. Compiled independently of the regular batch
  /// path; lands the native entry pointer in OsrEntryTable instead of
  /// FuncTable.
  struct OsrRequest {
    uint32_t FuncIdx;
    uint32_t LoopIdx;
    std::shared_ptr<const AST::Module> Mod;
    std::shared_ptr<void *[]> FuncTable;
    std::shared_ptr<void *[]> OsrEntryTable;
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
  /// in Seen_, bounded by BfsMaxDepth_ and MaxBatchSize_. A callee is
  /// included if either its runtime counter is nonzero OR its static
  /// frequency from the enclosing body is >= StaticFreqHot_ (catches
  /// siblings of the first-tripping function that haven't yet run).
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
  std::queue<OsrRequest> OsrQueue_;
  std::set<std::pair<uintptr_t, uint32_t>> Seen_;
  /// Dedup key for OSR compiles: (FuncTablePtr, FuncIdx<<16 | LoopIdx).
  /// Separate from Seen_ so regular tier-2 promotion and OSR compilation
  /// don't block each other.
  std::set<std::pair<uintptr_t, uint64_t>> SeenOsr_;
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
  // Static-frequency inclusion floor. A callee whose body is reached by
  // >= this many direct Call instructions in the enclosing caller is
  // batched even when its runtime counter is still 0 at tier-up time.
  // `2` excludes accidental singletons (error-path stubs called once)
  // while catching anything the LLVM inliner would obviously want —
  // sightglass-strong hot-body counts start in the single digits
  // (ge25519_add: 5 in f8) and reach hundreds (__multi3: 805 in f8).
  static constexpr uint32_t StaticFreqHot_ = 2;

  // Telemetry aggregates (dumped at shutdown).
  uint64_t StatBatches_ = 0;
  uint64_t StatSingletons_ = 0;
  uint64_t StatWalkupHits_ = 0;
  uint64_t StatBatchMemberSum_ = 0;
};

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
