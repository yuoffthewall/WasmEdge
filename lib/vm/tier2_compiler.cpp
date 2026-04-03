// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/tier2_compiler.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include "vm/jit_symbol_registry.h"
#include <spdlog/spdlog.h>

// dstogov/ir headers (for loading serialized IR text)
extern "C" {
#include "ir.h"
}

// LLVM C API headers
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/OrcEE.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <cstdio>
#include <string>

namespace WasmEdge::VM {

struct Tier2Compiler::Impl {
  LLVMTargetMachineRef TM = nullptr;
  char *Triple = nullptr;

  ~Impl() {
    if (TM)
      LLVMDisposeTargetMachine(TM);
    if (Triple)
      LLVMDisposeMessage(Triple);
  }
};

Tier2Compiler::Tier2Compiler() noexcept : P(std::make_unique<Impl>()) {
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();

  // Create a native TargetMachine for optimization passes and codegen.
  P->Triple = LLVMGetDefaultTargetTriple();
  LLVMTargetRef Target = nullptr;
  char *Err = nullptr;
  if (!LLVMGetTargetFromTriple(P->Triple, &Target, &Err)) {
    P->TM = LLVMCreateTargetMachine(Target, P->Triple, "generic", "",
                                     LLVMCodeGenLevelDefault,
                                     LLVMRelocPIC, LLVMCodeModelJITDefault);
  } else {
    spdlog::error("tier2: failed to get target for {}: {}", P->Triple,
                  Err ? Err : "(null)");
    LLVMDisposeMessage(Err);
  }
}
Tier2Compiler::~Tier2Compiler() noexcept = default;

// ---------------------------------------------------------------------------
// Minimal ir_loader callbacks for reloading serialized IR text.
// The loader only needs func_init (to ir_init the context) and func_process
// (to capture the parsed result).  All other callbacks are optional.
// ---------------------------------------------------------------------------
struct Tier2Loader {
  ir_loader Base;
  ir_ctx *Result; // Set by func_process — the caller must ir_free + delete.
  uint8_t RetType; // ir_type from tier-1 (ir_save doesn't encode it).
};

static bool tier2_func_init(ir_loader *loader, ir_ctx *ctx, const char *) {
  (void)loader;
  ir_init(ctx, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN,
          IR_INSNS_LIMIT_MIN);
  return true;
}

static bool tier2_func_process(ir_loader *loader, ir_ctx *ctx, const char *) {
  auto *L = reinterpret_cast<Tier2Loader *>(loader);

  // ir_save doesn't encode ret_type; ir_load sets it to -1 (unset).
  // Use the ret_type captured from tier-1's ir_ctx before ir_jit_compile.
  if (ctx->ret_type == static_cast<ir_type>(-1)) {
    ctx->ret_type = static_cast<ir_type>(L->RetType);
  }

  // Shallow-copy the ctx so that ir_load's subsequent ir_free(&ctx) doesn't
  // destroy the data we need.  We null the original's pointers to prevent
  // double-free.
  auto *Copy = new (std::nothrow) ir_ctx;
  if (!Copy)
    return false;
  std::memcpy(Copy, ctx, sizeof(ir_ctx));
  // Zero the original so ir_free in ir_load.c is a no-op.
  std::memset(ctx, 0, sizeof(ir_ctx));
  L->Result = Copy;
  return true;
}

static bool tier2_external_func_dcl(ir_loader *, const char *, uint32_t,
                                     ir_type, uint32_t, const uint8_t *) {
  return true; // Accept all external function declarations.
}

static bool tier2_external_sym_dcl(ir_loader *, const char *, uint32_t) {
  return true;
}

/// Load serialized IR text into a fresh ir_ctx.  Caller owns the returned
/// ir_ctx and must ir_free + delete it.
static ir_ctx *loadIRText(const std::string &IRText, uint8_t RetType) {
  ir_loader_init();

  Tier2Loader L{};
  L.Base.default_func_flags = IR_FUNCTION | IR_OPT_FOLDING;
  L.Base.func_init = tier2_func_init;
  L.Base.func_process = tier2_func_process;
  L.Base.external_func_dcl = tier2_external_func_dcl;
  L.Base.external_sym_dcl = tier2_external_sym_dcl;
  L.Result = nullptr;
  L.RetType = RetType;

  FILE *F = fmemopen(const_cast<char *>(IRText.data()), IRText.size(), "r");
  if (!F) {
    ir_loader_free();
    return nullptr;
  }

  ir_load_safe(&L.Base, F);
  fclose(F);
  ir_loader_free();

  return L.Result;
}

/// Convert a dstogov/ir ir_ctx* to LLVM IR text via ir_emit_llvm.
/// Runs the full scheduling pipeline (GCM + schedule + block ordering) on a
/// fresh context so that instructions are placed in dominating blocks.
static bool emitLLVMIR(ir_ctx *Ctx, const std::string &FuncName,
                       std::string &OutIR) {
  ir_build_def_use_lists(Ctx);
  if (!ir_build_cfg(Ctx)
   || !ir_build_dominators_tree(Ctx)
   || !ir_find_loops(Ctx)
   || !ir_gcm(Ctx)
   || !ir_schedule(Ctx)
   || !ir_schedule_blocks(Ctx)) {
    spdlog::error("tier2: IR scheduling passes failed for {}", FuncName);
    return false;
  }

  char *Buf = nullptr;
  size_t BufSize = 0;
  FILE *MemStream = open_memstream(&Buf, &BufSize);
  if (!MemStream) {
    spdlog::error("tier2: open_memstream failed");
    return false;
  }

  int Ret = ir_emit_llvm(Ctx, FuncName.c_str(), MemStream);
  fclose(MemStream);

  if (!Ret || !Buf) {
    spdlog::error("tier2: ir_emit_llvm failed for {}", FuncName);
    free(Buf);
    return false;
  }

  OutIR.assign(Buf, BufSize);
  free(Buf);
  return true;
}

// ---------------------------------------------------------------------------
// Register all JIT helper symbols as absolute symbols in the LLJIT's main
// JITDylib so LLVM-compiled code can resolve external calls.
// ---------------------------------------------------------------------------
static void registerSymbolsWithLLJIT(LLVMOrcLLJITRef J) {
  const auto &Registry = getJitSymbolRegistry();
  if (Registry.empty())
    return;

  auto ES = LLVMOrcLLJITGetExecutionSession(J);
  auto MainJD = LLVMOrcLLJITGetMainJITDylib(J);

  std::vector<LLVMOrcCSymbolMapPair> Pairs;
  Pairs.reserve(Registry.size());

  for (const auto &[Name, Addr] : Registry) {
    LLVMOrcCSymbolMapPair Pair;
    Pair.Name = LLVMOrcExecutionSessionIntern(ES, Name.c_str());
    Pair.Sym.Address = reinterpret_cast<LLVMOrcJITTargetAddress>(Addr);
    Pair.Sym.Flags.GenericFlags = LLVMJITSymbolGenericFlagsExported;
    Pair.Sym.Flags.TargetFlags = 0;
    Pairs.push_back(Pair);
  }

  auto MU = LLVMOrcAbsoluteSymbols(Pairs.data(), Pairs.size());
  if (auto Err = LLVMOrcJITDylibDefine(MainJD, MU)) {
    auto Msg = LLVMGetErrorMessage(Err);
    spdlog::error("tier2: failed to define absolute symbols: {}", Msg);
    LLVMDisposeErrorMessage(Msg);
  }
}

// ---------------------------------------------------------------------------
// Tier2Compiler::compile
// ---------------------------------------------------------------------------
Expect<Tier2CompileResult> Tier2Compiler::compile(const std::string &IRTextIn,
                                                   const std::string &FuncName,
                                                   uint8_t RetType,
                                                   unsigned OptLevel) {
  if (IRTextIn.empty()) {
    spdlog::warn("tier2: empty IR text for {}", FuncName);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // 1. Reload IR text into a fresh ir_ctx, then emit LLVM IR from it.
  ir_ctx *Ctx = loadIRText(IRTextIn, RetType);
  if (!Ctx) {
    spdlog::warn("tier2: failed to reload IR text for {}", FuncName);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  std::string IRText;
  bool EmitOk = emitLLVMIR(Ctx, FuncName, IRText);
  ir_free(Ctx);
  ::operator delete(Ctx);

  if (!EmitOk) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  if (std::getenv("WASMEDGE_TIER2_DUMP_IR")) {
    // Also dump the raw serialized IR text for debugging.
    std::string RawPath = "/tmp/tier2_" + FuncName + ".ir";
    if (FILE *F = fopen(RawPath.c_str(), "w")) {
      fwrite(IRTextIn.data(), 1, IRTextIn.size(), F);
      fclose(F);
    }
    std::string DumpPath = "/tmp/tier2_" + FuncName + ".ll";
    if (FILE *F = fopen(DumpPath.c_str(), "w")) {
      fwrite(IRText.data(), 1, IRText.size(), F);
      fclose(F);
      spdlog::info("tier2: wrote LLVM IR for {} to {}", FuncName, DumpPath);
    }
  }

  // 2. Create a ThreadSafeContext, get its underlying LLVMContext, and parse
  //    the LLVM IR text into a Module within that context.
  LLVMOrcThreadSafeContextRef TSCtx = LLVMOrcCreateNewThreadSafeContext();
  LLVMContextRef LLCtx = LLVMOrcThreadSafeContextGetContext(TSCtx);

  LLVMMemoryBufferRef MemBuf = LLVMCreateMemoryBufferWithMemoryRangeCopy(
      IRText.data(), IRText.size(), "tier2_ir");

  LLVMModuleRef Mod = nullptr;
  char *ErrMsg = nullptr;
  if (LLVMParseIRInContext(LLCtx, MemBuf, &Mod, &ErrMsg)) {

    spdlog::error("tier2: LLVMParseIRInContext failed for {}: {}", FuncName,
                  ErrMsg ? ErrMsg : "(null)");
    LLVMDisposeMessage(ErrMsg);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // 2a. Set target triple and data layout so optimization passes work correctly.
  if (P->TM) {
    LLVMSetTarget(Mod, P->Triple);
    auto *DL = LLVMCreateTargetDataLayout(P->TM);
    LLVMSetModuleDataLayout(Mod, DL);
    LLVMDisposeTargetData(DL);
  }

  // 2b. Verify the LLVM IR module. ir_emit_llvm can produce domination errors
  //     or other invalid IR that would crash LLVM's codegen.
  {
    char *VerifyMsg = nullptr;
    if (LLVMVerifyModule(Mod, LLVMReturnStatusAction, &VerifyMsg)) {
  
      spdlog::warn("tier2: LLVM verification failed for {}: {}", FuncName,
                    VerifyMsg ? VerifyMsg : "(null)");
      LLVMDisposeMessage(VerifyMsg);
      LLVMDisposeModule(Mod);
      LLVMOrcDisposeThreadSafeContext(TSCtx);
      return Unexpect(ErrCode::Value::RuntimeError);
    }
    LLVMDisposeMessage(VerifyMsg);
  }

  // Bail out if shutdown was signaled (avoid entering long LLVM calls).
  if (isShutdown()) {
    LLVMDisposeModule(Mod);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // 3. Run LLVM optimization passes.
  {
    char PassStr[32];
    std::snprintf(PassStr, sizeof(PassStr), "default<O%u>",
                  OptLevel > 3 ? 3u : OptLevel);

    LLVMPassBuilderOptionsRef PBO = LLVMCreatePassBuilderOptions();
    // Disable auto-vectorization: wasm JIT functions are typically small and
    // don't benefit from it. Also avoids LLVM 18 ISel bugs with scalable
    // vector types on x86-64 (report_fatal_error in EVT::getSizeInBits).
    LLVMPassBuilderOptionsSetLoopVectorization(PBO, 0);
    LLVMPassBuilderOptionsSetSLPVectorization(PBO, 0);
    if (auto Err = LLVMRunPasses(Mod, PassStr, P->TM, PBO)) {
      auto Msg = LLVMGetErrorMessage(Err);
      spdlog::warn("tier2: LLVMRunPasses warning for {}: {}", FuncName, Msg);
      LLVMDisposeErrorMessage(Msg);
    }
    LLVMDisposePassBuilderOptions(PBO);
  }

  // Bail out if shutdown was signaled (avoid ISel codegen which can crash).
  if (isShutdown()) {
    LLVMDisposeModule(Mod);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // 4. Create an ORC LLJIT and add the module.
  LLVMOrcLLJITRef J = nullptr;
  if (auto Err = LLVMOrcCreateLLJIT(&J, nullptr)) {

    auto Msg = LLVMGetErrorMessage(Err);
    spdlog::error("tier2: LLVMOrcCreateLLJIT failed: {}", Msg);
    LLVMDisposeErrorMessage(Msg);
    LLVMDisposeModule(Mod);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // Register external JIT helper symbols before adding the module.
  registerSymbolsWithLLJIT(J);

  // Wrap module in ThreadSafeModule (takes ownership of Mod) and add to LLJIT.
  {
    LLVMOrcThreadSafeModuleRef TSMod =
        LLVMOrcCreateNewThreadSafeModule(Mod, TSCtx);
    // TSMod now owns Mod. TSCtx is shared; we still hold our ref.

    auto MainJD = LLVMOrcLLJITGetMainJITDylib(J);
    if (auto Err = LLVMOrcLLJITAddLLVMIRModule(J, MainJD, TSMod)) {
  
      auto Msg = LLVMGetErrorMessage(Err);
      spdlog::error("tier2: addLLVMIRModule failed for {}: {}", FuncName, Msg);
      LLVMDisposeErrorMessage(Msg);
      LLVMOrcDisposeThreadSafeModule(TSMod);
      LLVMOrcDisposeThreadSafeContext(TSCtx);
      LLVMOrcDisposeLLJIT(J);
      return Unexpect(ErrCode::Value::RuntimeError);
    }
    // On success, LLJIT owns TSMod.
  }

  // Last chance to bail out before codegen (ISel is where LLVM 18 bugs crash).
  if (isShutdown()) {
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    LLVMOrcDisposeLLJIT(J);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // 5. Look up the compiled function (triggers LLJIT codegen).
  LLVMOrcJITTargetAddress FuncAddr = 0;
  if (auto Err = LLVMOrcLLJITLookup(J, &FuncAddr, FuncName.c_str())) {

    auto Msg = LLVMGetErrorMessage(Err);
    spdlog::error("tier2: lookup failed for {}: {}", FuncName, Msg);
    LLVMDisposeErrorMessage(Msg);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    LLVMOrcDisposeLLJIT(J);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  if (!FuncAddr) {
    spdlog::error("tier2: lookup returned null for {}", FuncName);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    LLVMOrcDisposeLLJIT(J);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // Note: We intentionally leak J and TSCtx here — the compiled code lives
  // in LLJIT's memory.  Phase 3 (Tier2Manager) will properly own these.
  // For now, tier-2 functions live for the process lifetime anyway.

  Tier2CompileResult Result;
  Result.NativeFunc = reinterpret_cast<void *>(FuncAddr);
  Result.CodeSize = 0; // ORC doesn't expose code size directly.

  spdlog::info("tier2: compiled {} → {:#x}", FuncName,
               static_cast<uintptr_t>(FuncAddr));

  return Result;
}

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
