// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/tier2_manager.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include "vm/tier2_compiler.h"
#include <fmt/format.h>
#include <spdlog/spdlog.h>

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
                           std::shared_ptr<void *[]> FuncTable) noexcept {
  {
    std::lock_guard<std::mutex> Lock(Mu_);
    // Dedup: skip if already enqueued or compiled for this FuncTable+FuncIdx.
    auto Key = std::make_pair(reinterpret_cast<uintptr_t>(FuncTable.get()),
                              FuncIdx);
    if (Seen_.count(Key))
      return;
    Seen_.insert(Key);
    Queue_.push({FuncIdx, std::move(IRText), RetType, std::move(FuncTable)});
  }
  CV_.notify_one();
}

void Tier2Manager::workerLoop() {
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

    // Load IR text → ir_ctx → LLVM IR → optimize → native code.
    std::string FuncName = fmt::format("wasm_tier2_{:03d}", Req.FuncIdx);
    spdlog::info("tier2: starting compile for func {} ({})", Req.FuncIdx, FuncName);
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
