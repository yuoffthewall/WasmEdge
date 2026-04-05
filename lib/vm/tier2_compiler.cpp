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

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

/// Extract callee funcIdx values from an ir_ctx by walking CALL instructions.
/// Pattern: CALL → op2 (PROTO) → op1 (LOAD) → op1 (ADD(FuncTablePtr, CONST)).
/// CONST / sizeof(void*) = funcIdx.
static std::vector<uint32_t> extractCallees(ir_ctx *Ctx) {
  std::vector<uint32_t> Callees;
  if (!Ctx || Ctx->insns_count <= 1)
    return Callees;

  // Find the FuncTablePtr ref: it's the first LOAD of type ADDR whose
  // source is an ADD of a PARAM + CONST (env + offsetof(FuncTable)).
  // We don't need to identify it precisely — we just need the constant
  // operand from the ADD in the CALL's load chain.

  for (ir_ref i = 1; i < Ctx->insns_count; ++i) {
    ir_insn *Insn = &Ctx->ir_base[i];
    if (Insn->op != IR_CALL)
      continue;

    // op2 = function operand (should be PROTO)
    ir_ref FuncRef = ir_insn_op(Insn, 2);
    if (FuncRef <= 0 || FuncRef >= Ctx->insns_count)
      continue;
    ir_insn *ProtoInsn = &Ctx->ir_base[FuncRef];
    if (ProtoInsn->op != IR_PROTO)
      continue;

    // PROTO's op1 = the loaded function pointer (should be LOAD)
    ir_ref LoadRef = ProtoInsn->op1;
    if (LoadRef <= 0 || LoadRef >= Ctx->insns_count)
      continue;
    ir_insn *LoadInsn = &Ctx->ir_base[LoadRef];
    if (LoadInsn->op != IR_LOAD)
      continue;

    // LOAD's op2 = address (should be ADD)
    ir_ref AddrRef = LoadInsn->op2;
    if (AddrRef <= 0 || AddrRef >= Ctx->insns_count)
      continue;
    ir_insn *AddInsn = &Ctx->ir_base[AddrRef];
    if (AddInsn->op != IR_ADD)
      continue;

    // ADD has two operands. One should be FuncTablePtr (a LOAD from env),
    // the other a constant offset. Find the constant operand.
    ir_ref ConstRef = IR_UNUSED;
    if (IR_IS_CONST_REF(AddInsn->op1))
      ConstRef = AddInsn->op1;
    else if (IR_IS_CONST_REF(AddInsn->op2))
      ConstRef = AddInsn->op2;
    else
      continue;

    // Extract the constant value (address type = uintptr_t).
    ir_insn *ConstInsn = &Ctx->ir_base[ConstRef];
    uintptr_t Offset = ConstInsn->val.addr;
    if (Offset == 0 || Offset % sizeof(void *) != 0)
      continue;

    uint32_t FuncIdx = static_cast<uint32_t>(Offset / sizeof(void *));
    Callees.push_back(FuncIdx);
  }

  // Deduplicate.
  std::sort(Callees.begin(), Callees.end());
  Callees.erase(std::unique(Callees.begin(), Callees.end()), Callees.end());
  return Callees;
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

// ---------------------------------------------------------------------------
// Tier2Compiler::getCallees — public wrapper around extractCallees + loadIRText
// ---------------------------------------------------------------------------
std::vector<uint32_t> Tier2Compiler::getCallees(const std::string &IRText,
                                                uint8_t RetType) {
  ir_ctx *Ctx = loadIRText(IRText, RetType);
  if (!Ctx)
    return {};
  auto Result = extractCallees(Ctx);
  ir_free(Ctx);
  ::operator delete(Ctx);
  return Result;
}

// ---------------------------------------------------------------------------
// Phase 2: Rewrite indirect FuncTable calls to direct calls for batch members.
//
// Scans the LLVM IR text line-by-line for the pattern:
//   %tN = add i64 %tM, OFFSET          ; OFFSET = funcIdx * 8
//   %dN = inttoptr i64 %tN to ptr
//   %dM = load ptr, ptr %dN             ; load FuncTable[funcIdx]
//   ... (optional bitcast)
//   %dR = call x86_fastcallcc TYPE %dM(...)  ; indirect call
//
// When OFFSET matches a batch member, replaces the indirect call target
// with a direct @funcName reference.
// ---------------------------------------------------------------------------
static void rewriteIntraBatchCalls(
    std::string &IR,
    const std::unordered_map<uint64_t, std::string> &OffsetToName) {
  if (OffsetToName.empty())
    return;

  // Trim leading/trailing whitespace from an SSA name.
  auto Trim = [](const std::string &S) -> std::string {
    size_t Start = 0, End = S.size();
    while (Start < End && (S[Start] == ' ' || S[Start] == '\t'))
      ++Start;
    while (End > Start && (S[End - 1] == ' ' || S[End - 1] == '\t'))
      --End;
    return S.substr(Start, End - Start);
  };

  // Extract the LHS SSA name before '=' (trimmed).
  auto GetDest = [&Trim](const std::string &Line) -> std::string {
    auto EqPos = Line.find('=');
    if (EqPos == std::string::npos)
      return {};
    return Trim(Line.substr(0, EqPos));
  };

  // First pass: collect return types from define lines.
  // "define i32 @wasm_tier2_030(..." → funcRetType["@wasm_tier2_030"] = "i32"
  std::unordered_map<std::string, std::string> FuncRetType;
  {
    size_t P = 0;
    while (P < IR.size()) {
      size_t LE = IR.find('\n', P);
      if (LE == std::string::npos) LE = IR.size();
      auto DefPos = IR.find("define ", P);
      if (DefPos != std::string::npos && DefPos < LE) {
        // "define TYPE @name("
        auto AtPos = IR.find('@', DefPos);
        auto ParenPos = IR.find('(', DefPos);
        if (AtPos != std::string::npos && ParenPos != std::string::npos &&
            AtPos < ParenPos) {
          std::string Name = IR.substr(AtPos, ParenPos - AtPos);
          // Type is between "define " and " @"
          std::string TypeStr = Trim(IR.substr(DefPos + 7, AtPos - DefPos - 7));
          FuncRetType[Name] = TypeStr;
        }
      }
      P = LE + 1;
    }
  }

  // SSA name tracking maps (all keys/values are trimmed).
  std::unordered_set<std::string> FuncTablePtrInts; // ptrtoint of %d4
  std::unordered_map<std::string, uint64_t> AddResultToOffset;
  std::unordered_map<std::string, uint64_t> IntToPtrToOffset;
  std::unordered_map<std::string, std::string> PtrToFunc;
  std::string FuncTableSSA; // SSA name of FuncTablePtr (first load from env)

  std::string Result;
  Result.reserve(IR.size());
  uint32_t RewriteCount = 0;

  size_t Pos = 0;
  while (Pos < IR.size()) {
    size_t LineEnd = IR.find('\n', Pos);
    if (LineEnd == std::string::npos)
      LineEnd = IR.size();
    std::string Line = IR.substr(Pos, LineEnd - Pos);

    // Reset tracking at function boundaries to avoid cross-function pollution.
    if (Line.find("define ") != std::string::npos) {
      FuncTablePtrInts.clear();
      AddResultToOffset.clear();
      IntToPtrToOffset.clear();
      PtrToFunc.clear();
      FuncTableSSA.clear();
    }

    // Detect FuncTablePtr: first "load ptr, ptr %dN" after function start.
    // In our IR, it's always "%d4 = load ptr, ptr %d2" (env pointer deref).
    if (FuncTableSSA.empty() && Line.find("= load ptr, ptr ") != std::string::npos) {
      std::string Dest = GetDest(Line);
      if (!Dest.empty())
        FuncTableSSA = Dest;
    }

    // Track ptrtoint of FuncTablePtr: "%tN_1 = ptrtoint ptr %d4 to i64"
    {
      auto PIP = Line.find("= ptrtoint ptr ");
      if (PIP != std::string::npos && !FuncTableSSA.empty()) {
        // Extract the source pointer after "= ptrtoint ptr "
        // "= ptrtoint ptr " is 15 chars
        std::string After = Line.substr(PIP + 15);
        auto Sp = After.find(' ');
        if (Sp != std::string::npos) {
          std::string Src = After.substr(0, Sp);
          if (Src == FuncTableSSA) {
            std::string Dest = GetDest(Line);
            if (!Dest.empty())
              FuncTablePtrInts.insert(Dest);
          }
        }
      }
    }

    // Pattern 1: %tN = add i64 %tN_1, OFFSET
    // Only match when the non-constant operand is a ptrtoint of FuncTablePtr.
    {
      auto P1 = Line.find("= add i64 ");
      if (P1 != std::string::npos) {
        std::string Dest = GetDest(Line);
        if (!Dest.empty()) {
          // Extract the two operands after "= add i64 "
          // "= add i64 " is 10 chars
          std::string After = Line.substr(P1 + 10);
          auto CommaPos = After.find(", ");
          if (CommaPos != std::string::npos) {
            std::string Op1 = Trim(After.substr(0, CommaPos));
            std::string Op2 = Trim(After.substr(CommaPos + 2));
            // One operand should be a FuncTablePtrInt, the other a number.
            std::string OffStr;
            bool IsFTAdd = false;
            if (FuncTablePtrInts.count(Op1)) {
              OffStr = Op2;
              IsFTAdd = true;
            } else if (FuncTablePtrInts.count(Op2)) {
              OffStr = Op1;
              IsFTAdd = true;
            }
            if (IsFTAdd) {
              bool IsNum = !OffStr.empty();
              for (char C : OffStr)
                if (C < '0' || C > '9') { IsNum = false; break; }
              if (IsNum) {
                uint64_t Off = std::stoull(OffStr);
                if (OffsetToName.count(Off))
                  AddResultToOffset[Dest] = Off;
              }
            }
          }
        }
      }
    }

    // Pattern 2: %dN = inttoptr i64 %tM to ptr
    {
      auto P2 = Line.find("= inttoptr i64 ");
      if (P2 != std::string::npos) {
        std::string Dest = GetDest(Line);
        if (!Dest.empty()) {
          // "= inttoptr i64 " is 15 chars
          std::string After = Line.substr(P2 + 15);
          auto Sp = After.find(' ');
          if (Sp != std::string::npos) {
            std::string Src = After.substr(0, Sp);
            auto It = AddResultToOffset.find(Src);
            if (It != AddResultToOffset.end())
              IntToPtrToOffset[Dest] = It->second;
          }
        }
      }
    }

    // Pattern 3: %dM = load ptr, ptr %dN
    {
      auto P3 = Line.find("= load ptr, ptr ");
      if (P3 != std::string::npos) {
        std::string Dest = GetDest(Line);
        if (!Dest.empty()) {
          // "= load ptr, ptr " is 16 chars
          std::string Src = Trim(Line.substr(P3 + 16));
          auto It = IntToPtrToOffset.find(Src);
          if (It != IntToPtrToOffset.end()) {
            auto NameIt = OffsetToName.find(It->second);
            if (NameIt != OffsetToName.end())
              PtrToFunc[Dest] = "@" + NameIt->second;
          }
        }
      }
    }

    // Pattern 3b: bitcast (pass-through)
    {
      auto P3b = Line.find("= bitcast ptr ");
      if (P3b != std::string::npos) {
        std::string Dest = GetDest(Line);
        if (!Dest.empty()) {
          // "= bitcast ptr " is 14 chars
          std::string Src = Line.substr(P3b + 14);
          auto ToPos = Src.find(" to ");
          if (ToPos != std::string::npos)
            Src = Src.substr(0, ToPos);
          Src = Trim(Src);
          auto It = PtrToFunc.find(Src);
          if (It != PtrToFunc.end())
            PtrToFunc[Dest] = It->second;
        }
      }
    }

    // Pattern 4: Replace indirect call target with direct call.
    // Handle return type mismatch (caller uses i64, callee defines i32/void).
    bool Replaced = false;
    {
      auto CallKwPos = Line.find("call x86_fastcallcc ");
      if (CallKwPos != std::string::npos) {
        auto ParenPos = Line.find('(');
        if (ParenPos != std::string::npos && ParenPos > 1) {
          size_t End = ParenPos;
          while (End > 0 && Line[End - 1] == ' ')
            --End;
          size_t Start = End;
          while (Start > 0 && Line[Start - 1] != ' ')
            --Start;
          std::string Target = Line.substr(Start, End - Start);
          auto It = PtrToFunc.find(Target);
          if (It != PtrToFunc.end()) {
            // Extract call return type.
            // "call x86_fastcallcc " is 20 chars from CallKwPos
            size_t TypeStart = CallKwPos + 20;
            size_t TypeEnd = Line.find(' ', TypeStart);
            std::string CallRetType =
                (TypeEnd != std::string::npos)
                    ? Line.substr(TypeStart, TypeEnd - TypeStart)
                    : "";
            auto RIt = FuncRetType.find(It->second);
            std::string DefRetType =
                (RIt != FuncRetType.end()) ? RIt->second : CallRetType;

            // Helper: strip "x86_fastcallcc " from a line (callee defines
            // use default CC, not fastcall).
            auto StripFastcall = [](std::string &S) {
              auto P = S.find("x86_fastcallcc ");
              if (P != std::string::npos)
                S.erase(P, 15); // "x86_fastcallcc " = 15 chars
            };

            if (CallRetType == DefRetType || DefRetType.empty()) {
              // Types match — simple rewrite.
              std::string NewLine =
                  Line.substr(0, Start) + It->second + Line.substr(End);
              StripFastcall(NewLine);
              Result += NewLine;
              Result += '\n';
              Replaced = true;
              ++RewriteCount;
            } else if (CallRetType == "i64" &&
                       (DefRetType == "i32" || DefRetType == "i16" ||
                        DefRetType == "i8")) {
              // Caller expects i64 but callee returns smaller int.
              // Rewrite call with callee's type, then zext to i64.
              std::string Dest = GetDest(Line);
              if (!Dest.empty()) {
                std::string TmpName = Dest + "_narrow";
                std::string CallLine = Line;
                // Replace dest with temp name.
                auto EqPos = CallLine.find('=');
                if (EqPos != std::string::npos)
                  CallLine = "\t" + TmpName + " " + CallLine.substr(EqPos);
                StripFastcall(CallLine);
                // Replace return type after "call ".
                auto CallKw2 = CallLine.find("call ");
                if (CallKw2 != std::string::npos) {
                  size_t TS = CallKw2 + 5;
                  size_t TE = CallLine.find(' ', TS);
                  if (TE != std::string::npos)
                    CallLine.replace(TS, TE - TS, DefRetType);
                }
                // Replace target.
                auto Paren2 = CallLine.find('(');
                if (Paren2 != std::string::npos) {
                  size_t E2 = Paren2;
                  while (E2 > 0 && CallLine[E2 - 1] == ' ') --E2;
                  size_t S2 = E2;
                  while (S2 > 0 && CallLine[S2 - 1] != ' ') --S2;
                  CallLine = CallLine.substr(0, S2) + It->second +
                             CallLine.substr(E2);
                }
                Result += CallLine;
                Result += '\n';
                Result += "\t" + Dest + " = zext " + DefRetType + " " +
                          TmpName + " to i64\n";
                Replaced = true;
                ++RewriteCount;
              }
            }
            // Other mismatches (void, float, double) — skip rewrite.
          }
        }
      }
    }

    if (!Replaced) {
      Result += Line;
      Result += '\n';
    }

    Pos = LineEnd + 1;
  }

  if (!IR.empty() && IR.back() != '\n' && !Result.empty() &&
      Result.back() == '\n')
    Result.pop_back();

  if (RewriteCount > 0)
    spdlog::info("tier2-batch: rewrote {} indirect → direct calls",
                 RewriteCount);

  IR = std::move(Result);
}

// ---------------------------------------------------------------------------
// Shared emit loader for multi-function LLVM IR emission.
// Prevents duplicate `declare` statements when emitting multiple functions
// to the same FILE stream.
// ---------------------------------------------------------------------------
struct SharedEmitLoader {
  ir_loader Base;
  std::unordered_set<std::string> DeclaredSyms;
};

static bool sharedHasSym(ir_loader *L, const char *Name) {
  auto *SL = reinterpret_cast<SharedEmitLoader *>(L);
  return SL->DeclaredSyms.count(Name) > 0;
}

static bool sharedAddSym(ir_loader *L, const char *Name, void *) {
  auto *SL = reinterpret_cast<SharedEmitLoader *>(L);
  SL->DeclaredSyms.insert(Name);
  return true;
}

// ---------------------------------------------------------------------------
// Tier2Compiler::compileBatch — compile multiple functions in one LLVM module.
// ---------------------------------------------------------------------------
Expect<std::vector<std::pair<uint32_t, void *>>>
Tier2Compiler::compileBatch(std::vector<BatchEntry> &Entries,
                            unsigned OptLevel) {
  if (Entries.empty())
    return Unexpect(ErrCode::Value::RuntimeError);

  // 1. Emit all functions to a single LLVM IR text buffer.
  SharedEmitLoader SL{};
  std::memset(&SL.Base, 0, sizeof(ir_loader));
  SL.Base.has_sym = sharedHasSym;
  SL.Base.add_sym = sharedAddSym;

  char *Buf = nullptr;
  size_t BufSize = 0;
  FILE *MemStream = open_memstream(&Buf, &BufSize);
  if (!MemStream) {
    spdlog::error("tier2-batch: open_memstream failed");
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // Track which functions we successfully emitted.
  std::vector<size_t> EmittedIdx;
  for (size_t i = 0; i < Entries.size(); ++i) {
    auto &E = Entries[i];
    ir_ctx *Ctx = loadIRText(E.IRText, E.RetType);
    if (!Ctx) {
      spdlog::warn("tier2-batch: failed to load IR for {}", E.FuncName);
      continue;
    }

    // Run scheduling passes.
    ir_build_def_use_lists(Ctx);
    bool Ok = ir_build_cfg(Ctx) && ir_build_dominators_tree(Ctx) &&
              ir_find_loops(Ctx) && ir_gcm(Ctx) && ir_schedule(Ctx) &&
              ir_schedule_blocks(Ctx);
    if (!Ok) {
      spdlog::warn("tier2-batch: scheduling failed for {}", E.FuncName);
      ir_free(Ctx);
      ::operator delete(Ctx);
      continue;
    }

    // Set shared loader to deduplicate declarations.
    Ctx->loader = &SL.Base;

    int Ret = ir_emit_llvm(Ctx, E.FuncName.c_str(), MemStream);
    Ctx->loader = nullptr; // Don't let ir_free touch the shared loader.
    ir_free(Ctx);
    ::operator delete(Ctx);

    if (!Ret) {
      spdlog::warn("tier2-batch: ir_emit_llvm failed for {}", E.FuncName);
      continue;
    }
    EmittedIdx.push_back(i);
  }

  fclose(MemStream);

  if (EmittedIdx.empty()) {
    free(Buf);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  std::string IRText(Buf, BufSize);
  free(Buf);

  // 1b. Rewrite intra-batch indirect calls to direct calls (Phase 2).
  // Build a map of offset -> funcName for batch members.
  std::unordered_map<uint64_t, std::string> OffsetToName;
  for (size_t idx : EmittedIdx) {
    auto &E = Entries[idx];
    uint64_t Offset = static_cast<uint64_t>(E.FuncIdx) * sizeof(void *);
    OffsetToName[Offset] = E.FuncName;
  }
  rewriteIntraBatchCalls(IRText, OffsetToName);

  if (std::getenv("WASMEDGE_TIER2_DUMP_IR")) {
    std::string DumpPath = "/tmp/tier2_batch_" +
                           Entries[EmittedIdx[0]].FuncName + ".ll";
    if (FILE *F = fopen(DumpPath.c_str(), "w")) {
      fwrite(IRText.data(), 1, IRText.size(), F);
      fclose(F);
      spdlog::info("tier2-batch: wrote batch LLVM IR to {}", DumpPath);
    }
  }

  // 2. Parse into one LLVM module.
  LLVMOrcThreadSafeContextRef TSCtx = LLVMOrcCreateNewThreadSafeContext();
  LLVMContextRef LLCtx = LLVMOrcThreadSafeContextGetContext(TSCtx);

  LLVMMemoryBufferRef MemBuf = LLVMCreateMemoryBufferWithMemoryRangeCopy(
      IRText.data(), IRText.size(), "tier2_batch_ir");

  LLVMModuleRef Mod = nullptr;
  char *ErrMsg = nullptr;
  if (LLVMParseIRInContext(LLCtx, MemBuf, &Mod, &ErrMsg)) {
    spdlog::error("tier2-batch: LLVMParseIRInContext failed: {}",
                  ErrMsg ? ErrMsg : "(null)");
    LLVMDisposeMessage(ErrMsg);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // 2a. Set target triple and data layout.
  if (P->TM) {
    LLVMSetTarget(Mod, P->Triple);
    auto *DL = LLVMCreateTargetDataLayout(P->TM);
    LLVMSetModuleDataLayout(Mod, DL);
    LLVMDisposeTargetData(DL);
  }

  // 2b. Verify.
  {
    char *VerifyMsg = nullptr;
    if (LLVMVerifyModule(Mod, LLVMReturnStatusAction, &VerifyMsg)) {
      spdlog::warn("tier2-batch: LLVM verification failed: {}",
                    VerifyMsg ? VerifyMsg : "(null)");
      LLVMDisposeMessage(VerifyMsg);
      LLVMDisposeModule(Mod);
      LLVMOrcDisposeThreadSafeContext(TSCtx);
      return Unexpect(ErrCode::Value::RuntimeError);
    }
    LLVMDisposeMessage(VerifyMsg);
  }

  if (isShutdown()) {
    LLVMDisposeModule(Mod);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // 3. Optimize.
  {
    char PassStr[32];
    std::snprintf(PassStr, sizeof(PassStr), "default<O%u>",
                  OptLevel > 3 ? 3u : OptLevel);

    LLVMPassBuilderOptionsRef PBO = LLVMCreatePassBuilderOptions();
    LLVMPassBuilderOptionsSetLoopVectorization(PBO, 0);
    LLVMPassBuilderOptionsSetSLPVectorization(PBO, 0);
    if (auto Err = LLVMRunPasses(Mod, PassStr, P->TM, PBO)) {
      auto Msg = LLVMGetErrorMessage(Err);
      spdlog::warn("tier2-batch: LLVMRunPasses warning: {}", Msg);
      LLVMDisposeErrorMessage(Msg);
    }
    LLVMDisposePassBuilderOptions(PBO);
  }

  if (isShutdown()) {
    LLVMDisposeModule(Mod);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // 4. Create LLJIT, add module, look up all functions.
  LLVMOrcLLJITRef J = nullptr;
  if (auto Err = LLVMOrcCreateLLJIT(&J, nullptr)) {
    auto Msg = LLVMGetErrorMessage(Err);
    spdlog::error("tier2-batch: LLVMOrcCreateLLJIT failed: {}", Msg);
    LLVMDisposeErrorMessage(Msg);
    LLVMDisposeModule(Mod);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  registerSymbolsWithLLJIT(J);

  {
    LLVMOrcThreadSafeModuleRef TSMod =
        LLVMOrcCreateNewThreadSafeModule(Mod, TSCtx);
    auto MainJD = LLVMOrcLLJITGetMainJITDylib(J);
    if (auto Err = LLVMOrcLLJITAddLLVMIRModule(J, MainJD, TSMod)) {
      auto Msg = LLVMGetErrorMessage(Err);
      spdlog::error("tier2-batch: addLLVMIRModule failed: {}", Msg);
      LLVMDisposeErrorMessage(Msg);
      LLVMOrcDisposeThreadSafeModule(TSMod);
      LLVMOrcDisposeThreadSafeContext(TSCtx);
      LLVMOrcDisposeLLJIT(J);
      return Unexpect(ErrCode::Value::RuntimeError);
    }
  }

  if (isShutdown()) {
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    LLVMOrcDisposeLLJIT(J);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // 5. Look up each function by name.
  std::vector<std::pair<uint32_t, void *>> Results;
  for (size_t idx : EmittedIdx) {
    auto &E = Entries[idx];
    LLVMOrcJITTargetAddress FuncAddr = 0;
    if (auto Err = LLVMOrcLLJITLookup(J, &FuncAddr, E.FuncName.c_str())) {
      auto Msg = LLVMGetErrorMessage(Err);
      spdlog::warn("tier2-batch: lookup failed for {}: {}", E.FuncName, Msg);
      LLVMDisposeErrorMessage(Msg);
      continue;
    }
    if (FuncAddr) {
      Results.emplace_back(E.FuncIdx, reinterpret_cast<void *>(FuncAddr));
      spdlog::info("tier2-batch: compiled {} → {:#x}", E.FuncName,
                   static_cast<uintptr_t>(FuncAddr));
    }
  }

  // Intentionally leak J and TSCtx — compiled code lives in LLJIT's memory.

  if (Results.empty())
    return Unexpect(ErrCode::Value::RuntimeError);

  return Results;
}

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
