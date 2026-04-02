// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "executor/executor.h"

#include "common/errinfo.h"
#include "common/spdlog.h"

#ifdef WASMEDGE_BUILD_IR_JIT
#include "vm/ir_builder.h"
#include "vm/ir_jit_engine.h"
#endif

#include <cstdint>
#include <string_view>

namespace WasmEdge {
namespace Executor {

// Instantiate module instance. See "include/executor/Executor.h".
Expect<std::unique_ptr<Runtime::Instance::ModuleInstance>>
Executor::instantiate(Runtime::StoreManager &StoreMgr, const AST::Module &Mod,
                      std::optional<std::string_view> Name) {
  // Check the module is validated.
  if (unlikely(!Mod.getIsValidated())) {
    spdlog::error(ErrCode::Value::NotValidated);
    spdlog::error(ErrInfo::InfoAST(ASTNodeAttr::Module));
    return Unexpect(ErrCode::Value::NotValidated);
  }

  // Create the stack manager.
  Runtime::StackManager StackMgr;

  // Check is module name duplicated when trying to registration.
  if (Name.has_value()) {
    const auto *FindModInst = StoreMgr.findModule(Name.value());
    if (FindModInst != nullptr) {
      spdlog::error(ErrCode::Value::ModuleNameConflict);
      spdlog::error(ErrInfo::InfoAST(ASTNodeAttr::Module));
      return Unexpect(ErrCode::Value::ModuleNameConflict);
    }
  }

  // Insert the module instance to store manager and retrieve instance.
  std::unique_ptr<Runtime::Instance::ModuleInstance> ModInst;
  if (Name.has_value()) {
    ModInst = std::make_unique<Runtime::Instance::ModuleInstance>(Name.value());
  } else {
    ModInst = std::make_unique<Runtime::Instance::ModuleInstance>("");
  }

  // Instantiate Function Types in Module Instance. (TypeSec)
  for (auto &SubType : Mod.getTypeSection().getContent()) {
    // Copy defined types to module instance.
    ModInst->addDefinedType(SubType);
  }

  auto ReportModuleError = [&StoreMgr, &ModInst](auto E) {
    spdlog::error(ErrInfo::InfoAST(ASTNodeAttr::Module));
    StoreMgr.recycleModule(std::move(ModInst));
    return E;
  };

  auto ReportError = [&StoreMgr, &ModInst](ASTNodeAttr Attr) {
    return [Attr, &StoreMgr, &ModInst](auto E) {
      spdlog::error(ErrInfo::InfoAST(Attr));
      spdlog::error(ErrInfo::InfoAST(ASTNodeAttr::Module));
      StoreMgr.recycleModule(std::move(ModInst));
      return E;
    };
  };

  // Instantiate ImportSection and do import matching. (ImportSec)
  const AST::ImportSection &ImportSec = Mod.getImportSection();
  EXPECTED_TRY(instantiate(
                   [&StoreMgr](std::string_view ModName)
                       -> const WasmEdge::Runtime::Instance::ModuleInstance * {
                     return StoreMgr.findModule(ModName);
                   },
                   *ModInst, ImportSec)
                   .map_error(ReportError(ASTNodeAttr::Sec_Import)));

  // Instantiate Functions in module. (FunctionSec, CodeSec)
  const AST::FunctionSection &FuncSec = Mod.getFunctionSection();
  const AST::CodeSection &CodeSec = Mod.getCodeSection();
  // This function will always success.
  instantiate(*ModInst, FuncSec, CodeSec);

  // Instantiate MemorySection (MemorySec)
  const AST::MemorySection &MemSec = Mod.getMemorySection();
  // This function will always success.
  instantiate(*ModInst, MemSec);

  // Instantiate TagSection (TagSec)
  const AST::TagSection &TagSec = Mod.getTagSection();
  // This function will always success.
  instantiate(*ModInst, TagSec);

  // Push a new frame {ModInst, locals:none}
  StackMgr.pushFrame(ModInst.get(), AST::InstrView::iterator(), 0, 0);

  // Instantiate GlobalSection (GlobalSec)
  const AST::GlobalSection &GlobSec = Mod.getGlobalSection();
  EXPECTED_TRY(instantiate(StackMgr, *ModInst, GlobSec)
                   .map_error(ReportError(ASTNodeAttr::Sec_Global)));

  // Instantiate TableSection (TableSec)
  const AST::TableSection &TabSec = Mod.getTableSection();
  EXPECTED_TRY(instantiate(StackMgr, *ModInst, TabSec)
                   .map_error(ReportError(ASTNodeAttr::Sec_Table)));

  // Instantiate ExportSection (ExportSec)
  const AST::ExportSection &ExportSec = Mod.getExportSection();
  // This function will always success.
  instantiate(*ModInst, ExportSec);

  // Instantiate ElementSection (ElemSec)
  const AST::ElementSection &ElemSec = Mod.getElementSection();
  EXPECTED_TRY(instantiate(StackMgr, *ModInst, ElemSec)
                   .map_error(ReportError(ASTNodeAttr::Sec_Element)));

  // Instantiate DataSection (DataSec)
  const AST::DataSection &DataSec = Mod.getDataSection();
  EXPECTED_TRY(instantiate(StackMgr, *ModInst, DataSec)
                   .map_error(ReportError(ASTNodeAttr::Sec_Data)));

  // Initialize table instances
  EXPECTED_TRY(initTable(StackMgr, ElemSec)
                   .map_error(ReportError(ASTNodeAttr::Sec_Element)));

  // Initialize memory instances
  EXPECTED_TRY(initMemory(StackMgr, DataSec)
                   .map_error(ReportError(ASTNodeAttr::Sec_Data)));

#ifdef WASMEDGE_BUILD_IR_JIT
  // IR JIT compilation: Compile instantiated functions to native code.
  // This happens after all sections are instantiated but before start function.
  // Skip if ForceInterpreter is set (interpreter-only mode).
  // Skip if module is already AOT-compiled (LLVM .so): functions are
  // CompiledFunction; re-compiling with IR JIT would overwrite them and can hang.
  const auto &CodeSegsAOTCheck = CodeSec.getContent();
  const bool isAOTModule =
      !CodeSegsAOTCheck.empty() && CodeSegsAOTCheck[0].getSymbol();
  if (!Conf.getRuntimeConfigure().isForceInterpreter() && !isAOTModule) {
    VM::IRJitEngine &IREngine = getIRJitEngine();
    VM::WasmToIRBuilder IRBuilder;
    
    // Collect global types for global.get/set instructions
    std::vector<ValType> GlobalTypes;
    for (const auto *GlobInst : ModInst->getGlobalInstances()) {
      GlobalTypes.push_back(GlobInst->getGlobalType().getValType());
    }
    
    // Collect function types for call instructions (indexed by function index)
    std::vector<const AST::FunctionType *> FuncTypes;
    for (const auto *FuncInst : ModInst->getFunctionInstances()) {
      FuncTypes.push_back(&FuncInst->getFuncType());
    }

    // Collect type section entries (indexed by type index) for call_indirect
    std::vector<const AST::FunctionType *> TypeSection;
    for (auto &SubType : Mod.getTypeSection().getContent()) {
      TypeSection.push_back(
          &SubType.getCompositeType().getFuncType());
    }
    
    // Get number of imported functions (skip these - they're not wasm functions)
    uint32_t ImportFuncNum = 0;
    for (const auto &ImpDesc : Mod.getImportSection().getContent()) {
      if (ImpDesc.getExternalType() == ExternalType::Function) {
        ImportFuncNum++;
      }
    }
    // Tier-2: read tier-up thresholds from env vars.
    uint32_t Tier2Threshold = 0;
    uint32_t Tier2LoopThreshold = 0;
    if (const char *E = std::getenv("WASMEDGE_TIER2_ENABLE")) {
      if (E[0] == '1' && E[1] == '\0') {
        Tier2Threshold = 10000;
        Tier2LoopThreshold = 1000;
        if (const char *T = std::getenv("WASMEDGE_TIER2_THRESHOLD")) {
          Tier2Threshold = static_cast<uint32_t>(std::atoi(T));
        }
        if (const char *T = std::getenv("WASMEDGE_TIER2_LOOP_THRESHOLD")) {
          Tier2LoopThreshold = static_cast<uint32_t>(std::atoi(T));
        }
        spdlog::debug("IR JIT tier-2 enabled: threshold={}, loop_threshold={}",
                      Tier2Threshold, Tier2LoopThreshold);
      }
    }

    const auto &CodeSegs = CodeSec.getContent();
    // ----------------------------------------------------------------
    // Pre-pass: determine which functions are safe to JIT-compile.
    // A function is NOT safe to JIT if it directly calls an imported (host)
    // function, because imported functions have nullptr in the JIT func table.
    // Transitively, a function that calls a non-JIT function is also unsafe
    // (the JIT code would call through nullptr and segfault).
    // ----------------------------------------------------------------
    uint32_t TotalDefined = static_cast<uint32_t>(CodeSegs.size());
    std::vector<bool> SkipJit(ImportFuncNum + TotalDefined, false);

    // All imported functions are inherently non-JIT.
    for (uint32_t i = 0; i < ImportFuncNum; i++) {
      SkipJit[i] = true;
    }

    // Mark functions that start with unreachable (trap stubs).
    for (uint32_t ci = 0; ci < TotalDefined; ci++) {
      auto Instrs = CodeSegs[ci].getExpr().getInstrs();
      if (!Instrs.empty() &&
          Instrs.begin()->getOpCode() == OpCode::Unreachable) {
        SkipJit[ImportFuncNum + ci] = true;
      }
    }

    // Transitive skip removed: the IR builder now routes calls to imports
    // through jit_host_call and call_indirect through the same trampoline,
    // so functions that call imports or use call_indirect can be JIT-compiled.

    // Log skip summary.
    {
      uint32_t SkipCount = 0;
      for (uint32_t ci = 0; ci < TotalDefined; ci++) {
        if (SkipJit[ImportFuncNum + ci]) SkipCount++;
      }
      if (SkipCount > 0) {
        spdlog::info("IR JIT: skipping {}/{} funcs (call non-JIT targets)",
                     SkipCount, TotalDefined);
      }
    }

    // Compile each defined (non-imported) wasm function
    // We use code segments from AST directly since FuncInst may already be
    // CompiledFunction (LLVM AOT) without accessible instructions.
    const auto &TypeIdxs = Mod.getFunctionSection().getContent();
    uint32_t FuncIdx = ImportFuncNum;
    uint32_t SuccessCount = 0;
    uint32_t InitFailCount = 0, BuildFailCount = 0, CompileFailCount = 0;
    uint32_t CodeIdx = 0;
    for (const auto &CodeSeg : CodeSegs) {
      auto *FuncInst = ModInst->unsafeGetFunction(FuncIdx);
      
      if (!FuncInst) {
        CodeIdx++;
        FuncIdx++;
        continue;
      }

      // Skip functions marked by the pre-pass (imports, trap stubs,
      // or functions that transitively call non-JIT targets).
      if (SkipJit[FuncIdx]) {
        spdlog::debug("IR JIT: skip func {} (non-JIT call target)", FuncIdx);
        CodeIdx++;
        FuncIdx++;
        continue;
      }
      
      // Get function type from the type section
      uint32_t TypeIdx = TypeIdxs[CodeIdx];
      auto TypeRes = ModInst->getType(TypeIdx);
      if (!TypeRes) {
        CodeIdx++;
        FuncIdx++;
        continue;
      }
      const AST::FunctionType &FuncType = (*TypeRes)->getCompositeType().getFuncType();
      
      // Get locals and instructions from AST code segment (not FuncInst)
      auto Locals = CodeSeg.getLocals();
      auto Instrs = CodeSeg.getExpr().getInstrs();
      
      // Create InstrView from instruction vector
      std::vector<AST::Instruction> InstrVec(Instrs.begin(), Instrs.end());
      
      {
        // Build IR
        IRBuilder.reset();
        IRBuilder.setFuncIdx(FuncIdx);

        // Tier-2: set call-counter threshold if tier-2 is enabled.
        // Pre-scan for loops to choose threshold.
        if (Tier2Threshold > 0) {
          bool FuncHasLoop = false;
          for (const auto &I : InstrVec) {
            if (I.getOpCode() == OpCode::Loop) {
              FuncHasLoop = true;
              break;
            }
          }
          IRBuilder.setTierUpThreshold(
              FuncHasLoop ? Tier2LoopThreshold : Tier2Threshold);
        }

        auto InitRes = IRBuilder.initialize(FuncType, Locals);
        if (!InitRes.has_value()) {
          InitFailCount++;
          CodeIdx++;
          FuncIdx++;
          continue;
        }
        // IMPORTANT: Set module context AFTER initialize() since initialize() calls reset()
        IRBuilder.setModuleFunctions(FuncTypes);
        IRBuilder.setModuleTypes(TypeSection);
        IRBuilder.setModuleGlobals(GlobalTypes);
        IRBuilder.setImportFuncNum(ImportFuncNum);
        
        // Pre-scan to find max call args for shared buffer allocation
        {
          uint32_t MaxArgs = 0;
          for (const auto &I : InstrVec) {
            auto IOp = I.getOpCode();
            if (IOp == OpCode::Call) {
              uint32_t Idx = I.getTargetIndex();
              if (Idx < FuncTypes.size()) {
                auto N = static_cast<uint32_t>(FuncTypes[Idx]->getParamTypes().size());
                if (N > MaxArgs) MaxArgs = N;
              }
            } else if (IOp == OpCode::Call_indirect) {
              uint32_t TIdx = I.getTargetIndex();
              if (TIdx < TypeSection.size()) {
                auto N = static_cast<uint32_t>(TypeSection[TIdx]->getParamTypes().size());
                if (N > MaxArgs) MaxArgs = N;
              }
            }
          }
          IRBuilder.setMaxCallArgs(MaxArgs);
        }
        
        spdlog::debug("IR JIT: Building func {}", FuncIdx);
        auto BuildRes = IRBuilder.buildFromInstructions(InstrVec);
        if (!BuildRes.has_value()) {
          spdlog::info("IR JIT: func {} build failed: {}", FuncIdx,
                       static_cast<uint32_t>(BuildRes.error()));
          BuildFailCount++;
          CodeIdx++;
          FuncIdx++;
          continue;
        }
        
        // Compile to native code
        auto CompRes = IREngine.compile(IRBuilder.getIRContext());
        if (!CompRes.has_value()) {
          spdlog::info("IR JIT: func {} compile failed", FuncIdx);
          CompileFailCount++;
          CodeIdx++;
          FuncIdx++;
          continue;
        }
        // Upgrade function to IR JIT.
        // Preserve IR graph when tier-2 is enabled (needed for ir_emit_llvm).
        ir_ctx *PreservedGraph = nullptr;
        if (Tier2Threshold > 0) {
          PreservedGraph = IRBuilder.detachIRContext();
        }
        FuncInst->upgradeToIRJit(CompRes->NativeFunc, CompRes->CodeSize,
                                 PreservedGraph);
        SuccessCount++;
      }
      CodeIdx++;
      FuncIdx++;
    }
    spdlog::info("IR JIT stats: init_fail={}, build_fail={}, compile_fail={}",
                 InitFailCount, BuildFailCount, CompileFailCount);
    
    spdlog::info("IR JIT: Compiled {}/{} functions successfully", 
                 SuccessCount, FuncIdx - ImportFuncNum);
  }
#endif
  (void)Conf; // Suppress unused warning when IR JIT disabled

  // Instantiate StartSection (StartSec)
  const AST::StartSection &StartSec = Mod.getStartSection();
  if (StartSec.getContent()) {
    // Get the module instance from ID.
    ModInst->setStartIdx(*StartSec.getContent());

    // Get function instance.
    const auto *FuncInst = ModInst->getStartFunc();

    // Execute instruction.
    EXPECTED_TRY(
        runFunction(StackMgr, *FuncInst, {}).map_error(ReportModuleError));
  }

  // Pop Frame.
  StackMgr.popFrame();

  // For the named modules, register it into the store.
  if (Name.has_value()) {
    StoreMgr.registerModule(ModInst.get());
  }

  return ModInst;
}

} // namespace Executor
} // namespace WasmEdge
