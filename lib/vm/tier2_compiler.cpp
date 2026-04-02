// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/tier2_compiler.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include "vm/jit_symbol_registry.h"
#include <spdlog/spdlog.h>

// dstogov/ir headers
extern "C" {
#include "ir.h"
}

// LLVM C API headers
#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/OrcEE.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace WasmEdge::VM {

struct Tier2Compiler::Impl {};

Tier2Compiler::Tier2Compiler() noexcept : P(std::make_unique<Impl>()) {
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();
}
Tier2Compiler::~Tier2Compiler() noexcept = default;

// ---------------------------------------------------------------------------
// Emit LLVM IR text from ir_ctx* via ir_emit_llvm → open_memstream.
// ---------------------------------------------------------------------------
static bool emitLLVMIR(ir_ctx *Ctx, const std::string &FuncName,
                       std::string &OutIR) {
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
Expect<Tier2CompileResult> Tier2Compiler::compile(ir_ctx *Ctx,
                                                   const std::string &FuncName,
                                                   unsigned OptLevel) {
  // 1. Emit LLVM IR text from the ir_ctx* graph.
  std::string IRText;
  if (!emitLLVMIR(Ctx, FuncName, IRText)) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  if (std::getenv("WASMEDGE_TIER2_DUMP_IR")) {
    // Write to /tmp for inspection (spdlog may truncate).
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

  // 3. Run LLVM optimization passes.
  {
    char PassStr[32];
    std::snprintf(PassStr, sizeof(PassStr), "default<O%u>",
                  OptLevel > 3 ? 3u : OptLevel);

    LLVMPassBuilderOptionsRef PBO = LLVMCreatePassBuilderOptions();
    if (auto Err = LLVMRunPasses(Mod, PassStr, nullptr, PBO)) {
      auto Msg = LLVMGetErrorMessage(Err);
      spdlog::warn("tier2: LLVMRunPasses warning for {}: {}", FuncName, Msg);
      LLVMDisposeErrorMessage(Msg);
    }
    LLVMDisposePassBuilderOptions(PBO);
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

  // 5. Look up the compiled function.
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
