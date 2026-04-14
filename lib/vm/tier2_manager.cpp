// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/tier2_manager.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include "ast/module.h"
#include "vm/tier2_compiler.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace WasmEdge::VM {

namespace {

/// Is this value type something our scalar-only MVP thunk can marshal?
/// Supported: i32, i64, f32, f64. Skipped: v128, reference types.
bool isScalarMarshalable(const ValType &VT) noexcept {
  switch (VT.getCode()) {
  case TypeCode::I32:
  case TypeCode::I64:
  case TypeCode::F32:
  case TypeCode::F64:
    return true;
  default:
    return false;
  }
}

/// Return true if \p FuncIdx refers to a defined function whose signature
/// is entirely scalar (≤ 64 bit) and single-return-or-void. This is the
/// MVP scope limit for tier-2 promotion.
bool isPromotable(const AST::Module &Mod, uint32_t FuncIdx,
                  uint32_t ImportFuncNum) noexcept {
  if (FuncIdx < ImportFuncNum) {
    return false; // imports not promotable
  }
  const auto &FuncSec = Mod.getFunctionSection().getContent();
  const uint32_t DefinedIdx = FuncIdx - ImportFuncNum;
  if (DefinedIdx >= FuncSec.size()) {
    return false;
  }
  const uint32_t TypeIdx = FuncSec[DefinedIdx];
  const auto &TypeSec = Mod.getTypeSection().getContent();
  if (TypeIdx >= TypeSec.size()) {
    return false;
  }
  const auto &FT = TypeSec[TypeIdx].getCompositeType().getFuncType();
  if (FT.getReturnTypes().size() > 1) {
    return false; // multi-return: skipped for MVP
  }
  for (const auto &PT : FT.getParamTypes()) {
    if (!isScalarMarshalable(PT)) {
      return false;
    }
  }
  for (const auto &RT : FT.getReturnTypes()) {
    if (!isScalarMarshalable(RT)) {
      return false;
    }
  }
  return true;
}

/// Walk the hot function's instruction stream and collect direct Call
/// target indices (module-wide funcIdx space).
std::vector<uint32_t> extractCalleeIndices(const AST::Module &Mod,
                                           uint32_t FuncIdx,
                                           uint32_t ImportFuncNum) noexcept {
  std::vector<uint32_t> Result;
  if (FuncIdx < ImportFuncNum) {
    return Result;
  }
  const auto &CodeSec = Mod.getCodeSection().getContent();
  const uint32_t DefinedIdx = FuncIdx - ImportFuncNum;
  if (DefinedIdx >= CodeSec.size()) {
    return Result;
  }
  const auto Instrs = CodeSec[DefinedIdx].getExpr().getInstrs();
  std::unordered_set<uint32_t> Seen;
  for (auto It = Instrs.begin(); It != Instrs.end(); ++It) {
    if (It->getOpCode() == OpCode::Call) {
      uint32_t Target = It->getTargetIndex();
      if (Seen.insert(Target).second) {
        Result.push_back(Target);
      }
    }
  }
  return Result;
}

} // namespace

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

void Tier2Manager::enqueue(uint32_t FuncIdx,
                           std::shared_ptr<const AST::Module> Mod,
                           std::shared_ptr<void *[]> FuncTable) noexcept {
  if (!Mod || !FuncTable) {
    return;
  }
  {
    std::lock_guard<std::mutex> Lock(Mu_);
    auto Key = std::make_pair(reinterpret_cast<uintptr_t>(FuncTable.get()),
                              FuncIdx);
    if (Seen_.count(Key))
      return;
    Seen_.insert(Key);
    Queue_.push({FuncIdx, std::move(Mod), std::move(FuncTable)});
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
    if (!Req.Mod || !Req.FuncTable)
      continue;
    if (Shutdown_.load(std::memory_order_acquire)) {
      WorkerDone_.store(true, std::memory_order_release);
      return;
    }

    const AST::Module &Mod = *Req.Mod;

    // Count import functions once per request.
    uint32_t ImportFuncNum = 0;
    for (const auto &ImpDesc : Mod.getImportSection().getContent()) {
      if (ImpDesc.getExternalType() == ExternalType::Function) {
        ++ImportFuncNum;
      }
    }

    // Scope filter: the hot function must itself be scalar-only.
    if (!isPromotable(Mod, Req.FuncIdx, ImportFuncNum)) {
      spdlog::debug("tier2: skip func {} (unsupported signature)",
                    Req.FuncIdx);
      continue;
    }

    // Build batch: hot function + direct callees (depth 1, scalar-only).
    constexpr size_t MaxBatchSize = 12;
    const auto FTKey = reinterpret_cast<uintptr_t>(Req.FuncTable.get());
    std::vector<uint32_t> Batch;
    Batch.reserve(MaxBatchSize);
    Batch.push_back(Req.FuncIdx);

    auto Callees = extractCalleeIndices(Mod, Req.FuncIdx, ImportFuncNum);
    for (uint32_t C : Callees) {
      if (Batch.size() >= MaxBatchSize)
        break;
      if (C == Req.FuncIdx)
        continue;
      if (!isPromotable(Mod, C, ImportFuncNum))
        continue;
      {
        std::lock_guard<std::mutex> Lock(Mu_);
        if (Seen_.count(std::make_pair(FTKey, C)))
          continue;
      }
      Batch.push_back(C);
    }

    spdlog::info("tier2: starting batch compile for func {} with {} "
                 "function(s)",
                 Req.FuncIdx, Batch.size());

    auto BatchResult = Compiler.compileBatch(Batch, Mod, 2);
    if (!BatchResult) {
      spdlog::warn("tier2: batch compile failed for func {}", Req.FuncIdx);
      continue;
    }

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
  }
}

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
