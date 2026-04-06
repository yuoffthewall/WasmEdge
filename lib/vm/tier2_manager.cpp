// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/tier2_manager.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include "vm/tier2_compiler.h"
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <pthread.h>
#include <sched.h>

namespace WasmEdge::VM {

Tier2Manager::Tier2Manager() noexcept {
  Worker_ = std::thread([this] { workerLoop(); });
}

Tier2Manager::~Tier2Manager() noexcept {
  shutdown();
  if (Worker_.joinable())
    Worker_.join();
}

void Tier2Manager::shutdown() noexcept {
  {
    std::lock_guard<std::mutex> Lock(Mu_);
    Shutdown_.store(true, std::memory_order_release);
  }
  CV_.notify_one();
}

void Tier2Manager::enqueue(uint32_t FuncIdx, std::string IRText,
                           uint8_t RetType,
                           std::shared_ptr<void *[]> FuncTable,
                           std::shared_ptr<ModuleFuncMap> ModFuncs) noexcept {
  {
    std::lock_guard<std::mutex> Lock(Mu_);
    // Dedup: skip if already enqueued or compiled for this FuncTable+FuncIdx.
    auto Key = std::make_pair(reinterpret_cast<uintptr_t>(FuncTable.get()),
                              FuncIdx);
    if (Seen_.count(Key))
      return;
    Seen_.insert(Key);
    Queue_.push({FuncIdx, std::move(IRText), RetType, std::move(FuncTable),
                 std::move(ModFuncs)});
  }
  CV_.notify_one();
}

void Tier2Manager::workerLoop() {
  // Lower background thread priority so LLVM compilation doesn't steal CPU
  // from the main execution thread.
  {
    struct sched_param Param = {};
    Param.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &Param);
  }

  Tier2Compiler Compiler;
  Compiler.setShutdownFlag(&Shutdown_);

  // Debug: limit number of tier-2 compilations via env var.
  uint32_t MaxCompilations = UINT32_MAX;
  if (const char *E = ::getenv("WASMEDGE_TIER2_MAX_COMPILE"))
    MaxCompilations = static_cast<uint32_t>(std::atoi(E));
  uint32_t CompileCount = 0;

  while (true) {
    Request Req;
    {
      std::unique_lock<std::mutex> Lock(Mu_);
      CV_.wait(Lock, [this] {
        return !Queue_.empty() || Shutdown_.load(std::memory_order_acquire);
      });
      if (Shutdown_.load(std::memory_order_acquire)) {
        WorkerDone_.store(true, std::memory_order_release);
        return;
      }
      Req = std::move(Queue_.front());
      Queue_.pop();
    }

    if (CompileCount >= MaxCompilations)
      continue;

    if (Req.IRText.empty()) {
      spdlog::warn("tier2: func {} has empty IR text", Req.FuncIdx);
      continue;
    }

    // Check shutdown again before starting a potentially-crashing compilation.
    if (Shutdown_.load(std::memory_order_acquire)) {
      WorkerDone_.store(true, std::memory_order_release);
      return;
    }

    std::string FuncName = fmt::format("wasm_tier2_{:03d}", Req.FuncIdx);
    auto FTKey = reinterpret_cast<uintptr_t>(Req.FuncTable.get());

    // Try batch compilation if we have the module function map.
    if (Req.ModFuncs && !Req.ModFuncs->empty()) {
      // Extract callees from the hot function.
      auto Callees = Tier2Compiler::getCallees(Req.IRText, Req.RetType);

      // Build batch: hot function + its direct callees (depth 1).
      std::vector<Tier2Compiler::BatchEntry> Batch;
      Batch.push_back({Req.FuncIdx, Req.IRText, FuncName, Req.RetType});

      constexpr size_t MaxBatchSize = 12;
      for (uint32_t CalleeIdx : Callees) {
        if (Batch.size() >= MaxBatchSize)
          break;
        // Skip if already compiled or enqueued.
        auto Key = std::make_pair(FTKey, CalleeIdx);
        if (Seen_.count(Key))
          continue;
        // Look up callee in module function map.
        auto It = Req.ModFuncs->find(CalleeIdx);
        if (It == Req.ModFuncs->end())
          continue; // Import or no IR text.
        const auto &[IRText, RetType] = It->second;
        if (IRText.empty())
          continue;

        std::string CalleeName =
            fmt::format("wasm_tier2_{:03d}", CalleeIdx);
        Batch.push_back({CalleeIdx, IRText, CalleeName, RetType});
      }

      spdlog::info("tier2: starting batch compile for func {} ({}) with {} "
                    "functions",
                    Req.FuncIdx, FuncName, Batch.size());

      auto BatchResult = Compiler.compileBatch(Batch, 2);
      if (BatchResult) {
        // Mark all batch members as seen and swap their FuncTable entries.
        for (auto &[FIdx, NativePtr] : *BatchResult) {
          {
            std::lock_guard<std::mutex> Lock(Mu_);
            Seen_.insert(std::make_pair(FTKey, FIdx));
          }
          Req.FuncTable.get()[FIdx] = NativePtr;
          Tier2Count_.fetch_add(1, std::memory_order_relaxed);
          ++CompileCount;
          spdlog::info("tier2: upgraded func {} → tier-2 ({:#x})", FIdx,
                       reinterpret_cast<uintptr_t>(NativePtr));
        }
        continue;
      }
      // Batch failed — fall through to single-function compilation.
      spdlog::warn("tier2: batch compile failed, falling back to single for {}",
                    FuncName);
    }

    // Single-function compilation (original path / fallback).
    spdlog::info("tier2: starting compile for func {} ({})", Req.FuncIdx,
                 FuncName);
    auto Result = Compiler.compile(Req.IRText, FuncName, Req.RetType, 2);
    if (!Result) {
      spdlog::warn("tier2: compilation failed for func {} ({})", Req.FuncIdx,
                    FuncName);
      continue;
    }

    // Swap the FuncTable pointer. This is a single pointer store which is
    // naturally atomic on x86-64/aarch64. The old tier-1 code stays alive
    // (owned by FunctionInstance) so in-flight calls complete safely.
    Req.FuncTable.get()[Req.FuncIdx] = Result->NativeFunc;
    Tier2Count_.fetch_add(1, std::memory_order_relaxed);
    ++CompileCount;

    spdlog::info("tier2: upgraded func {} → tier-2 ({:#x})", Req.FuncIdx,
                 reinterpret_cast<uintptr_t>(Result->NativeFunc));
  }
}

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
