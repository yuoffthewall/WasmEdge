// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/tier2_manager.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include "ast/module.h"
#include "vm/ir_jit_engine.h"
#include "vm/tier2_compiler.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <sstream>
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
  // Skip trap stubs: bodies that begin with `unreachable` are not emitted by
  // the LLVM frontend, so including them in a tier-2 batch would produce a
  // missing `f<Idx>` symbol when emitFwdThunk looks it up.
  const auto &CodeSec = Mod.getCodeSection().getContent();
  if (DefinedIdx < CodeSec.size()) {
    const auto Instrs = CodeSec[DefinedIdx].getExpr().getInstrs();
    auto It = Instrs.begin();
    if (It != Instrs.end() && It->getOpCode() == OpCode::Unreachable) {
      return false;
    }
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

uint32_t envAsU32(const char *Name, uint32_t Default) noexcept {
  if (const char *E = ::getenv(Name)) {
    int V = std::atoi(E);
    if (V >= 0)
      return static_cast<uint32_t>(V);
  }
  return Default;
}

} // namespace

Tier2Manager::Tier2Manager() noexcept {
  Tier2Threshold_ = envAsU32("WASMEDGE_TIER2_THRESHOLD", 1000);
  Worker_ = std::thread([this] { workerLoop(); });
}

Tier2Manager::~Tier2Manager() noexcept {
  shutdown();
  if (Worker_.joinable())
    Worker_.join();
  if (StatBatches_ > 0) {
    double MeanSize =
        static_cast<double>(StatBatchMemberSum_) / StatBatches_;
    spdlog::info(
        "tier2: shutdown stats: batches={} singletons={} walkup_hits={} "
        "mean_size={:.2f}",
        StatBatches_, StatSingletons_, StatWalkupHits_, MeanSize);
  }
}

void Tier2Manager::shutdown() noexcept {
  {
    std::lock_guard<std::mutex> Lock(Mu_);
    Shutdown_.store(true, std::memory_order_release);
  }
  CV_.notify_one();
}

const Tier2Manager::ModuleCG &
Tier2Manager::getOrBuildCGLocked(const AST::Module &Mod) noexcept {
  // Count imports + code size up front so we can validate any cached entry.
  // AST::Module* addresses may be reused across kernels when old modules
  // are destroyed and new ones allocate at the same heap slot; a stale hit
  // returns a CG sized for the wrong module and corrupts batching.
  uint32_t ImportFuncNum = 0;
  for (const auto &ImpDesc : Mod.getImportSection().getContent()) {
    if (ImpDesc.getExternalType() == ExternalType::Function) {
      ++ImportFuncNum;
    }
  }
  const auto &CodeSec = Mod.getCodeSection().getContent();

  auto It = CGCache_.find(&Mod);
  if (It != CGCache_.end()) {
    const auto &Existing = It->second;
    if (Existing.ImportFuncNum == ImportFuncNum &&
        Existing.Callees.size() == CodeSec.size()) {
      return Existing;
    }
    CGCache_.erase(It);
  }

  ModuleCG CG;
  CG.ImportFuncNum = ImportFuncNum;
  CG.Callees.resize(CodeSec.size());
  CG.Callers.resize(CodeSec.size());
  for (size_t DefinedIdx = 0; DefinedIdx < CodeSec.size(); ++DefinedIdx) {
    const uint32_t CallerFuncIdx =
        static_cast<uint32_t>(DefinedIdx) + CG.ImportFuncNum;
    const auto Instrs = CodeSec[DefinedIdx].getExpr().getInstrs();
    std::unordered_set<uint32_t> Seen;
    for (auto It2 = Instrs.begin(); It2 != Instrs.end(); ++It2) {
      if (It2->getOpCode() != OpCode::Call)
        continue;
      const uint32_t Target = It2->getTargetIndex();
      if (!Seen.insert(Target).second)
        continue;
      CG.Callees[DefinedIdx].push_back(Target);
      // Record reverse edge only if target is a defined (not imported) func.
      if (Target >= CG.ImportFuncNum) {
        const uint32_t TargetDef = Target - CG.ImportFuncNum;
        if (TargetDef < CG.Callers.size()) {
          CG.Callers[TargetDef].push_back(CallerFuncIdx);
        }
      }
    }
  }
  auto [It2, _] = CGCache_.emplace(&Mod, std::move(CG));
  return It2->second;
}

std::pair<uint32_t, uint32_t>
Tier2Manager::walkUpRootLocked(const AST::Module &Mod, const ModuleCG &CG,
                               uint32_t HotFuncIdx, uintptr_t FTKey,
                               const uint32_t *CallCounters) noexcept {
  if (CallCounters == nullptr) {
    return {HotFuncIdx, 0};
  }
  // A caller only counts as "warm" if it's itself within a fraction of
  // the tier-up threshold — i.e., it would realistically tier up on its
  // own. A looser floor (e.g. 1 invocation) would drag cold one-shot
  // parents into the batch and ruin inlining quality.
  const uint32_t WarmThreshold =
      std::max<uint32_t>(1, Tier2Threshold_ / std::max<uint32_t>(1, WarmDivisor_));
  uint32_t Root = HotFuncIdx;
  uint32_t Hops = 0;
  std::unordered_set<uint32_t> Visited;
  Visited.insert(Root);
  while (Hops < WalkupMaxDepth_) {
    if (Root < CG.ImportFuncNum)
      break;
    const uint32_t RootDef = Root - CG.ImportFuncNum;
    if (RootDef >= CG.Callers.size())
      break;
    const auto &Callers = CG.Callers[RootDef];
    uint32_t Best = UINT32_MAX;
    uint32_t BestCount = 0;
    for (uint32_t C : Callers) {
      if (C == Root)
        continue;
      if (Visited.count(C))
        continue;
      if (Seen_.count(std::make_pair(FTKey, C)))
        continue;
      if (!isPromotable(Mod, C, CG.ImportFuncNum))
        continue;
      uint32_t CCount = CallCounters[C];
      // Saturated sentinel means already-tier-upped; skip.
      if (CCount == UINT32_MAX)
        continue;
      if (CCount < WarmThreshold)
        continue;
      if (Best == UINT32_MAX || CCount > BestCount) {
        Best = C;
        BestCount = CCount;
      }
    }
    if (Best == UINT32_MAX)
      break;
    Root = Best;
    Visited.insert(Root);
    ++Hops;
  }
  return {Root, Hops};
}

std::vector<uint32_t>
Tier2Manager::bfsDownBatchLocked(const AST::Module &Mod, const ModuleCG &CG,
                                 uint32_t Root, uint32_t HotFuncIdx,
                                 uintptr_t FTKey,
                                 const uint32_t *CallCounters) noexcept {
  std::vector<uint32_t> Batch;
  Batch.reserve(MaxBatchSize_);
  std::unordered_set<uint32_t> InBatch;

  auto TryAdd = [&](uint32_t F) -> bool {
    if (Batch.size() >= MaxBatchSize_)
      return false;
    if (InBatch.count(F))
      return false;
    if (!isPromotable(Mod, F, CG.ImportFuncNum))
      return false;
    if (Seen_.count(std::make_pair(FTKey, F)))
      return false;
    Batch.push_back(F);
    InBatch.insert(F);
    return true;
  };

  // Dual-source BFS from Root AND HotFuncIdx. Root expansion captures
  // the parent + its direct callees (hot's siblings). Hot expansion
  // captures hot's own direct callees. Together they form the
  // inlining neighborhood of the hot function. Seeding both at depth 0
  // means both sources get equal BFS budget.
  std::deque<std::pair<uint32_t, uint32_t>> Frontier; // (funcIdx, depth)
  Frontier.push_back({Root, 0});
  if (HotFuncIdx != Root)
    Frontier.push_back({HotFuncIdx, 0});
  while (!Frontier.empty() && Batch.size() < MaxBatchSize_) {
    auto [F, D] = Frontier.front();
    Frontier.pop_front();
    if (!TryAdd(F))
      continue;
    if (D >= BfsMaxDepth_)
      continue;
    if (F < CG.ImportFuncNum)
      continue;
    const uint32_t DefinedIdx = F - CG.ImportFuncNum;
    if (DefinedIdx >= CG.Callees.size())
      continue;
    for (uint32_t C : CG.Callees[DefinedIdx]) {
      if (Batch.size() >= MaxBatchSize_)
        break;
      if (InBatch.count(C))
        continue;
      // Skip callees that were never actually called at runtime.
      // Static call edges include cold paths (error handlers, init
      // routines) that waste batch slots and LLVM compile time.
      if (CallCounters) {
        uint32_t CCount = CallCounters[C];
        if (CCount == 0)
          continue;
      }
      Frontier.push_back({C, D + 1});
    }
  }

  // Ensure the originally-hot function is in the batch even if BFS
  // didn't reach it (root choice + depth cap + batch cap). This
  // preserves the guarantee that every tier-up request promotes its
  // trigger function.
  if (!InBatch.count(HotFuncIdx)) {
    if (Batch.size() >= MaxBatchSize_ && !Batch.empty()) {
      // Make room by dropping the last added BFS leaf.
      uint32_t Dropped = Batch.back();
      Batch.pop_back();
      InBatch.erase(Dropped);
    }
    TryAdd(HotFuncIdx);
  }
  return Batch;
}

void Tier2Manager::enqueue(uint32_t FuncIdx,
                           std::shared_ptr<const AST::Module> Mod,
                           std::shared_ptr<void *[]> FuncTable,
                           const uint32_t *CallCounters) noexcept {
  if (!Mod || !FuncTable) {
    return;
  }
  Request Req;
  {
    std::lock_guard<std::mutex> Lock(Mu_);
    const auto FTKey = reinterpret_cast<uintptr_t>(FuncTable.get());
    if (Seen_.count(std::make_pair(FTKey, FuncIdx)))
      return;

    const ModuleCG &CG = getOrBuildCGLocked(*Mod);

    // The hot function itself must be scalar-promotable, otherwise we have
    // nothing to compile. Mark it Seen_ so we never retry it.
    if (!isPromotable(*Mod, FuncIdx, CG.ImportFuncNum)) {
      Seen_.insert(std::make_pair(FTKey, FuncIdx));
      spdlog::debug("tier2: skip func {} (unsupported signature)", FuncIdx);
      return;
    }

    auto [Root, Hops] =
        walkUpRootLocked(*Mod, CG, FuncIdx, FTKey, CallCounters);
    auto Batch =
        bfsDownBatchLocked(*Mod, CG, Root, FuncIdx, FTKey, CallCounters);
    if (Batch.empty()) {
      Seen_.insert(std::make_pair(FTKey, FuncIdx));
      return;
    }

    // Reserve every batch member against future tier-up triggers.
    // This is the core anti-fragmentation mechanism: once a function
    // neighborhood is captured in a batch, sibling leaf trips that
    // would otherwise become singletons (with no callers to batch
    // them with) are suppressed because the parent already pulled
    // them in here.
    for (uint32_t B : Batch) {
      Seen_.insert(std::make_pair(FTKey, B));
    }

    Req.HotFuncIdx = FuncIdx;
    Req.RootFuncIdx = Root;
    Req.WalkupHops = Hops;
    Req.Batch = std::move(Batch);
    Req.Mod = std::move(Mod);
    Req.FuncTable = std::move(FuncTable);
    Queue_.push(std::move(Req));
  }
  CV_.notify_one();
}

void Tier2Manager::enqueueOsr(
    uint32_t FuncIdx, uint32_t LoopIdx,
    std::shared_ptr<const AST::Module> Mod,
    std::shared_ptr<void *[]> FuncTable,
    std::shared_ptr<void *[]> OsrEntryTable) noexcept {
  if (!Mod || !FuncTable || !OsrEntryTable) {
    return;
  }
  if (LoopIdx >= OSR_MAX_LOOPS_PER_FUNC) {
    return;
  }
  OsrRequest Req;
  {
    std::lock_guard<std::mutex> Lock(Mu_);
    const auto FTKey = reinterpret_cast<uintptr_t>(FuncTable.get());
    const uint64_t OsrKey =
        (static_cast<uint64_t>(FuncIdx) << 32) | LoopIdx;
    if (SeenOsr_.count(std::make_pair(FTKey, OsrKey)))
      return;
    SeenOsr_.insert(std::make_pair(FTKey, OsrKey));

    // If the target function has already been tier-2 promoted at its
    // entry, the tier-1 body is dead — OSR into it would waste cycles.
    if (Seen_.count(std::make_pair(FTKey, FuncIdx)))
      return;

    Req.FuncIdx = FuncIdx;
    Req.LoopIdx = LoopIdx;
    Req.Mod = std::move(Mod);
    Req.FuncTable = std::move(FuncTable);
    Req.OsrEntryTable = std::move(OsrEntryTable);
    OsrQueue_.push(std::move(Req));
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
    OsrRequest OsrReq;
    bool HaveBatch = false;
    bool HaveOsr = false;
    {
      std::unique_lock<std::mutex> Lock(Mu_);
      CV_.wait(Lock, [this] {
        return !Queue_.empty() || !OsrQueue_.empty() ||
               Shutdown_.load(std::memory_order_acquire);
      });
      if (Shutdown_.load(std::memory_order_acquire)) {
        WorkerDone_.store(true, std::memory_order_release);
        return;
      }
      // Prefer regular batches first for fairness; OSR drains after.
      if (!Queue_.empty()) {
        Req = std::move(Queue_.front());
        Queue_.pop();
        HaveBatch = true;
      } else if (!OsrQueue_.empty()) {
        OsrReq = std::move(OsrQueue_.front());
        OsrQueue_.pop();
        HaveOsr = true;
      }
    }

    if (CompileCount >= MaxCompilations)
      continue;
    if (Shutdown_.load(std::memory_order_acquire)) {
      WorkerDone_.store(true, std::memory_order_release);
      return;
    }

    if (HaveBatch) {
      if (!Req.Mod || !Req.FuncTable || Req.Batch.empty())
        continue;

      const AST::Module &Mod = *Req.Mod;
      const auto FTKey = reinterpret_cast<uintptr_t>(Req.FuncTable.get());

      std::ostringstream Idxs;
      for (size_t I = 0; I < Req.Batch.size(); ++I) {
        if (I)
          Idxs << ",";
        Idxs << Req.Batch[I];
      }
      spdlog::info(
          "tier2: batch compile: root={} hot={} size={} walkup={} [{}]",
          Req.RootFuncIdx, Req.HotFuncIdx, Req.Batch.size(), Req.WalkupHops,
          Idxs.str());

      {
        std::lock_guard<std::mutex> Lock(Mu_);
        ++StatBatches_;
        StatBatchMemberSum_ += Req.Batch.size();
        if (Req.Batch.size() == 1)
          ++StatSingletons_;
        if (Req.WalkupHops > 0)
          ++StatWalkupHits_;
      }

      auto BatchResult = Compiler.compileBatch(Req.Batch, Mod, 2);
      if (!BatchResult) {
        spdlog::warn("tier2: batch compile failed for func {}",
                     Req.HotFuncIdx);
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
      continue;
    }

    if (HaveOsr) {
      if (!OsrReq.Mod || !OsrReq.FuncTable || !OsrReq.OsrEntryTable)
        continue;
      spdlog::info("tier2: OSR compile: func={} loop={}", OsrReq.FuncIdx,
                   OsrReq.LoopIdx);
      unsigned OsrOpt = 2;
      if (const char *E = ::getenv("WASMEDGE_OSR_OPT_LEVEL")) {
        OsrOpt = static_cast<unsigned>(std::atoi(E));
      }
      auto OsrResult = Compiler.compileOsrEntry(OsrReq.FuncIdx,
                                                OsrReq.LoopIdx,
                                                *OsrReq.Mod, OsrOpt);
      if (!OsrResult) {
        spdlog::warn("tier2: OSR compile failed for func {} loop {}",
                     OsrReq.FuncIdx, OsrReq.LoopIdx);
        continue;
      }
      void *Entry = *OsrResult;
      // Atomic store: the tier-1 IR polls this slot on each iteration
      // and the memory-order-release here pairs with the memory-order-
      // acquire load in emitLoopBackEdge().
      auto *Slot = reinterpret_cast<std::atomic<void *> *>(
          &OsrReq.OsrEntryTable.get()[OsrReq.FuncIdx *
                                          OSR_MAX_LOOPS_PER_FUNC +
                                      OsrReq.LoopIdx]);
      Slot->store(Entry, std::memory_order_release);
      ++CompileCount;
      spdlog::info("tier2: OSR entry installed: func={} loop={} ({:#x})",
                   OsrReq.FuncIdx, OsrReq.LoopIdx,
                   reinterpret_cast<uintptr_t>(Entry));
    }
  }
}

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
