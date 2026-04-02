// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/tier2_manager.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include "runtime/instance/function.h"
#include "runtime/instance/module.h"
#include "vm/tier2_compiler.h"
#include <fmt/format.h>
#include <set>
#include <spdlog/spdlog.h>

namespace WasmEdge::VM {

Tier2Manager::Tier2Manager() noexcept {
  Worker_ = std::thread([this] { workerLoop(); });
}

Tier2Manager::~Tier2Manager() noexcept {
  {
    std::lock_guard<std::mutex> Lock(Mu_);
    Shutdown_.store(true, std::memory_order_release);
  }
  CV_.notify_one();
  if (Worker_.joinable())
    Worker_.join();
}

void Tier2Manager::enqueue(uint32_t FuncIdx,
                           const Runtime::Instance::ModuleInstance *ModInst,
                           void **FuncTable) noexcept {
  {
    std::lock_guard<std::mutex> Lock(Mu_);
    // Dedup: skip if already enqueued or compiled.
    auto Key = std::make_pair(ModInst, FuncIdx);
    if (Seen_.count(Key))
      return;
    Seen_.insert(Key);
    Queue_.push({FuncIdx, ModInst, FuncTable});
  }
  CV_.notify_one();
}

void Tier2Manager::workerLoop() {
  Tier2Compiler Compiler;

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
      if (Shutdown_.load(std::memory_order_acquire) && Queue_.empty())
        return;
      Req = Queue_.front();
      Queue_.pop();
    }

    if (CompileCount >= MaxCompilations)
      continue;

    // Get the FunctionInstance for this funcIdx.
    auto Funcs = Req.ModInst->getFunctionInstances();
    if (Req.FuncIdx >= Funcs.size()) {
      spdlog::warn("tier2: funcIdx {} out of range ({})", Req.FuncIdx,
                    Funcs.size());
      continue;
    }

    const auto *FuncInst = Funcs[Req.FuncIdx];
    if (!FuncInst || !FuncInst->isIRJitFunction()) {
      spdlog::warn("tier2: func {} is not an IR JIT function", Req.FuncIdx);
      continue;
    }

    const auto &JitFunc = FuncInst->getIRJitFunc();
    if (JitFunc.IRText.empty()) {
      spdlog::warn("tier2: func {} has no preserved IR text", Req.FuncIdx);
      continue;
    }

    // Load IR text → ir_ctx → LLVM IR → optimize → native code.
    std::string FuncName = fmt::format("wasm_tier2_{:03d}", Req.FuncIdx);
    auto Result = Compiler.compile(JitFunc.IRText, FuncName, 2);
    if (!Result) {
      spdlog::warn("tier2: compilation failed for func {} ({})", Req.FuncIdx,
                    FuncName);
      continue;
    }

    // Swap the FuncTable pointer. This is a single pointer store which is
    // naturally atomic on x86-64/aarch64. The old tier-1 code stays alive
    // (owned by FunctionInstance) so in-flight calls complete safely.
    Req.FuncTable[Req.FuncIdx] = Result->NativeFunc;
    Tier2Count_.fetch_add(1, std::memory_order_relaxed);
    ++CompileCount;

    spdlog::info("tier2: upgraded func {} → tier-2 ({:#x})", Req.FuncIdx,
                 reinterpret_cast<uintptr_t>(Result->NativeFunc));
  }
}

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
