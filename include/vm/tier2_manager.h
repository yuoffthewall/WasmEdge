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
#include <string>
#include <thread>
#include <utility>

namespace WasmEdge {
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
  /// \param FuncIdx   Module function index.
  /// \param IRText    Serialized IR text for the function.
  /// \param RetType   ir_type of the function's return value.
  /// \param FuncTable Shared pointer to the FuncTable array (for live code swap).
  void enqueue(uint32_t FuncIdx, std::string IRText, uint8_t RetType,
               std::shared_ptr<void *[]> FuncTable) noexcept;

  /// Number of functions successfully recompiled to tier-2.
  uint32_t tier2Count() const noexcept {
    return Tier2Count_.load(std::memory_order_relaxed);
  }

private:
  struct Request {
    uint32_t FuncIdx;
    std::string IRText;
    uint8_t RetType;
    std::shared_ptr<void *[]> FuncTable;
  };

  void workerLoop();

  std::thread Worker_;
  std::mutex Mu_;
  std::condition_variable CV_;
  std::queue<Request> Queue_;
  std::set<std::pair<uintptr_t, uint32_t>> Seen_;
  std::atomic<bool> Shutdown_{false};
  std::atomic<bool> WorkerDone_{false};
  std::atomic<uint32_t> Tier2Count_{0};
};

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
