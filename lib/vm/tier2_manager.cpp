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
#include <unordered_map>
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
    // First pass: count static call frequency per target (multiset).
    std::unordered_map<uint32_t, uint32_t> Freq;
    for (auto It2 = Instrs.begin(); It2 != Instrs.end(); ++It2) {
      if (It2->getOpCode() != OpCode::Call)
        continue;
      ++Freq[It2->getTargetIndex()];
    }
    CG.Callees[DefinedIdx].reserve(Freq.size());
    for (auto &KV : Freq) {
      const uint32_t Target = KV.first;
      const uint32_t StaticFreq = KV.second;
      CG.Callees[DefinedIdx].push_back({Target, StaticFreq});
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
  // Ratio gate (sole floor): candidate ancestor must be at least
  // 1/RootHotRatioDen_ as hot as the leaf that tripped tier-up. Falls
  // through to (HotFuncIdx, 0) when nothing qualifies, letting BFS-down
  // anchor on the leaf itself.
  //
  // The leaf's counter has already been saturated to UINT32_MAX by
  // jit_tier_up_notify before we get here. We know it just crossed
  // Tier2Threshold_, so use that as the effective leaf count: it's a
  // lower bound (actual may have been higher before saturation) and
  // keeps the ratio arithmetic inside a sensible range.
  uint32_t LeafCount = CallCounters[HotFuncIdx];
  if (LeafCount == UINT32_MAX)
    LeafCount = Tier2Threshold_;
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
      // Ratio gate: CCount * RootHotRatioDen_ >= LeafCount.
      // Using uint64_t to avoid overflow at extreme leaf counts.
      if (static_cast<uint64_t>(CCount) * RootHotRatioDen_ < LeafCount)
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
                                 const uint32_t *CallCounters,
                                 bool SkipSeen) noexcept {
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
    if (!SkipSeen && Seen_.count(std::make_pair(FTKey, F)))
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
    for (const auto &[C, StaticFreq] : CG.Callees[DefinedIdx]) {
      if (Batch.size() >= MaxBatchSize_)
        break;
      if (InBatch.count(C))
        continue;
      // Inclusion is the OR of two signals: either the callee has
      // already run (dynamic counter nonzero) or the body of the
      // enclosing caller contains enough static call sites to it that
      // skipping it would leave the hottest caller with indirect
      // t1_thunk dispatches at those sites. The static gate closes the
      // bootstrap window where only the first-tripping callee has a
      // nonzero counter (e.g. ed25519: when f19 saturated first, all of
      // f8's other helpers were still at 0 and got dropped, yielding
      // [f8,f19] and then singletons for everyone else).
      const bool Dynamic =
          CallCounters != nullptr && CallCounters[C] != 0;
      const bool Static = StaticFreq >= StaticFreqHot_;
      if (!Dynamic && !Static)
        continue;
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
                           std::optional<uint32_t> LoopIdx,
                           std::shared_ptr<const AST::Module> Mod,
                           std::shared_ptr<void *[]> FuncTable,
                           std::shared_ptr<void *[]> OsrEntryTable,
                           const uint32_t *CallCounters) noexcept {
  if (!Mod || !FuncTable) {
    return;
  }
  const bool IsOsr = LoopIdx.has_value();
  if (IsOsr) {
    if (!OsrEntryTable || *LoopIdx >= OSR_MAX_LOOPS_PER_FUNC) {
      return;
    }
  }

  Request Req;
  {
    std::lock_guard<std::mutex> Lock(Mu_);
    const auto FTKey = reinterpret_cast<uintptr_t>(FuncTable.get());

    if (IsOsr) {
      const uint64_t OsrKey =
          (static_cast<uint64_t>(FuncIdx) << 32) | *LoopIdx;
      if (SeenOsr_.count(std::make_pair(FTKey, OsrKey)))
        return;
      SeenOsr_.insert(std::make_pair(FTKey, OsrKey));
      // Do NOT short-circuit when FuncIdx is already in Seen_: function-
      // entry swap only redirects *future* calls. The currently-running
      // tier-1 frame stays in the tier-1 body until the loop exits —
      // exactly the case OSR exists to rescue.
    } else {
      if (Seen_.count(std::make_pair(FTKey, FuncIdx)))
        return;
    }

    const ModuleCG &CG = getOrBuildCGLocked(*Mod);

    if (!isPromotable(*Mod, FuncIdx, CG.ImportFuncNum)) {
      // Mark the call-count path as Seen_ so we never retry. OSR is gated
      // by SeenOsr_ already, and emitFwdThunk would still bail at compile
      // time on non-scalar signatures.
      if (!IsOsr) {
        Seen_.insert(std::make_pair(FTKey, FuncIdx));
      }
      spdlog::debug("tier2: skip func {} (unsupported signature)", FuncIdx);
      return;
    }

    uint32_t Root = FuncIdx;
    uint32_t Hops = 0;
    if (!IsOsr) {
      std::tie(Root, Hops) =
          walkUpRootLocked(*Mod, CG, FuncIdx, FTKey, CallCounters);
    }

    // OSR uses SkipSeen=true so already-tier2'd callees still appear in
    // the inlining neighborhood (the running tier-1 frame needs the loop
    // entry regardless of FuncTable swaps). CallCounters is passed for
    // both triggers — by the time OSR fires (back-edge threshold reached)
    // most loop-body callees already have non-zero counters, and the
    // dynamic gate is what catches direct-call helpers whose static_freq
    // is only 1 (e.g. shootout-ctype char-class predicates: dropping
    // them from the OSR batch regressed ctype 0.71× vs the P1g baseline).
    auto Batch = bfsDownBatchLocked(*Mod, CG, Root, FuncIdx, FTKey,
                                    CallCounters,
                                    /*SkipSeen=*/IsOsr);
    if (Batch.empty()) {
      if (!IsOsr) {
        Seen_.insert(std::make_pair(FTKey, FuncIdx));
      }
      return;
    }

    if (!IsOsr) {
      // Reserve every batch member against future tier-up triggers.
      // Anti-fragmentation: once a neighborhood is captured, sibling
      // leaf trips that would become singletons are suppressed.
      for (uint32_t B : Batch) {
        Seen_.insert(std::make_pair(FTKey, B));
      }
    }

    Req.HotFuncIdx = FuncIdx;
    Req.RootFuncIdx = Root;
    Req.WalkupHops = Hops;
    Req.Batch = std::move(Batch);
    Req.Mod = std::move(Mod);
    Req.FuncTable = std::move(FuncTable);
    if (IsOsr) {
      Req.LoopEntries = {*LoopIdx};
      Req.OsrEntryTable = std::move(OsrEntryTable);
      OsrQueue_.push(std::move(Req));
    } else {
      Queue_.push(std::move(Req));
    }
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
        return !Queue_.empty() || !OsrQueue_.empty() ||
               Shutdown_.load(std::memory_order_acquire);
      });
      if (Shutdown_.load(std::memory_order_acquire)) {
        WorkerDone_.store(true, std::memory_order_release);
        return;
      }
      // OSR drains first; see header for rationale.
      if (!OsrQueue_.empty()) {
        Req = std::move(OsrQueue_.front());
        OsrQueue_.pop();
      } else if (!Queue_.empty()) {
        Req = std::move(Queue_.front());
        Queue_.pop();
      } else {
        continue;
      }
    }

    if (CompileCount >= MaxCompilations)
      continue;
    if (Shutdown_.load(std::memory_order_acquire)) {
      WorkerDone_.store(true, std::memory_order_release);
      return;
    }

    if (!Req.Mod || !Req.FuncTable || Req.Batch.empty())
      continue;
    const bool IsOsr = !Req.LoopEntries.empty();
    if (IsOsr && !Req.OsrEntryTable)
      continue;

    const AST::Module &Mod = *Req.Mod;
    const auto FTKey = reinterpret_cast<uintptr_t>(Req.FuncTable.get());

    std::ostringstream Idxs;
    for (size_t I = 0; I < Req.Batch.size(); ++I) {
      if (I)
        Idxs << ",";
      Idxs << Req.Batch[I];
    }
    if (IsOsr) {
      spdlog::info(
          "tier2-osr: compile: root={} loop={} size={} [{}]",
          Req.RootFuncIdx, Req.LoopEntries.front(),
          Req.Batch.size(), Idxs.str());
    } else {
      spdlog::info(
          "tier2: batch compile: root={} hot={} size={} walkup={} [{}]",
          Req.RootFuncIdx, Req.HotFuncIdx, Req.Batch.size(),
          Req.WalkupHops, Idxs.str());
      std::lock_guard<std::mutex> Lock(Mu_);
      ++StatBatches_;
      StatBatchMemberSum_ += Req.Batch.size();
      if (Req.Batch.size() == 1)
        ++StatSingletons_;
      if (Req.WalkupHops > 0)
        ++StatWalkupHits_;
    }

    unsigned OptLevel = 2;
    if (IsOsr) {
      if (const char *E = ::getenv("WASMEDGE_OSR_OPT_LEVEL")) {
        OptLevel = static_cast<unsigned>(std::atoi(E));
      }
    }

    auto Result = Compiler.compileRequest(Req.RootFuncIdx, Req.Batch,
                                          Req.LoopEntries, Mod, OptLevel);
    if (!Result) {
      spdlog::warn("tier2: compile failed for root func {}",
                   Req.RootFuncIdx);
      continue;
    }

    if (IsOsr) {
      const uint32_t OsrFuncIdx = Req.RootFuncIdx;
      for (auto &[L, Entry] : Result->OsrEntries) {
        // Atomic store: tier-1 polls this slot every back-edge; the
        // memory-order-release here pairs with the acquire load in
        // emitLoopBackEdge().
        auto *Slot = reinterpret_cast<std::atomic<void *> *>(
            &Req.OsrEntryTable.get()[OsrFuncIdx *
                                         OSR_MAX_LOOPS_PER_FUNC +
                                     L]);
        Slot->store(Entry, std::memory_order_release);
        ++CompileCount;
        spdlog::info("tier2-osr: entry installed: func={} loop={} ({:#x})",
                     OsrFuncIdx, L,
                     reinterpret_cast<uintptr_t>(Entry));
      }
    } else {
      for (auto &[FIdx, NativePtr] : Result->FwdThunks) {
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
}

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
