// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/tier2_compiler.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include "ast/module.h"
#include "common/configure.h"
#include "executor/executor.h"
#include "llvm/compiler.h"
#include "llvm/data.h"
#include "validator/validator.h"

#include <spdlog/spdlog.h>

#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// Tier-2 helpers defined in ir_jit_engine.cpp.
// wasmedge_tier2_get_jit_env: legacy accessor (ORC fallback path).
// wasmedge_tier2_get_jit_env_tls_offset: returns the byte offset of
//   the thread-local JitExecEnv* from the thread pointer (%fs:0 on
//   x86_64-linux). Used to emit a direct %fs:OFFSET inline asm load
//   in t1_thunks instead of calling through an ORC absolute symbol.
extern "C" void *wasmedge_tier2_get_jit_env(void);
extern "C" ptrdiff_t wasmedge_tier2_get_jit_env_tls_offset(void);
// Returns the byte offset of Executor::ExecutionContext (a thread_local
// struct) from %fs:0. Used to emit a direct address computation in
// fwd_thunks instead of calling the ORC-bound wasmedge_tier2_get_exec_ctx.
extern "C" ptrdiff_t wasmedge_tier2_get_exec_ctx_tls_offset(void);

// Debug helper gated by WASMEDGE_TIER2_TRACE_FUNC env var. Prints
// (func_idx, arg0..arg3) on each fwd_thunk entry. Bound via ORC absolute
// symbol and called from inside emitFwdThunk when the env var is set.
namespace {
int g_tier2_trace_func = -1;
}
extern "C" void wasmedge_tier2_trace_thunk(uint32_t FuncIdx, uint64_t A0,
                                            uint64_t A1, uint64_t A2,
                                            uint64_t A3) {
  if (static_cast<int>(FuncIdx) != g_tier2_trace_func) {
    return;
  }
  std::fprintf(stderr,
               "[tier2-trace] f%u(a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx)\n",
               FuncIdx, A0, A1, A2, A3);

  // Also dump the memory bytes at a0 for len = min(a1, 32). This is f30
  // specific (__fwritex buf/len).
  void *Ctx = WasmEdge::Executor::Executor::getThreadLocalExecutionContextPtr();
  if (!Ctx) {
    std::fprintf(stderr, "[tier2-trace-mem] ctx=null\n");
    return;
  }
  // ExecutionContext first field is Memories (see executor.h).
  // STABLE:    uint8_t *const * Memories
  // NON-STABLE: uint8_t **const * Memories
  void *MemField = *static_cast<void **>(Ctx);
  if (!MemField) {
    std::fprintf(stderr, "[tier2-trace-mem] MemField=null\n");
    return;
  }
  uint8_t *Base = nullptr;
#if defined(WASMEDGE_ALLOCATOR_IS_STABLE) && WASMEDGE_ALLOCATOR_IS_STABLE
  Base = static_cast<uint8_t *const *>(MemField)[0];
#else
  void *Slot = static_cast<void *const *>(MemField)[0];
  if (Slot) Base = *static_cast<uint8_t **>(Slot);
#endif
  std::fprintf(stderr, "[tier2-trace-mem] MemField=%p Base=%p\n", MemField,
               (void *)Base);
  if (!Base) return;
  uint32_t Buf = static_cast<uint32_t>(A0);
  uint32_t Len = static_cast<uint32_t>(A1);
  if (Len > 32) Len = 32;
  char Pr[64] = {0};
  char Hx[100] = {0};
  for (uint32_t i = 0; i < Len && i < 32; ++i) {
    unsigned char c = Base[Buf + i];
    Pr[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    std::snprintf(Hx + i * 3, 4, "%02x ", c);
  }
  std::fprintf(stderr, "[tier2-trace-mem] f%u mem[0x%x..+%u]='%s' hex=%s\n",
               FuncIdx, Buf, Len, Pr, Hx);
}

namespace WasmEdge::VM {

namespace {

/// Build a synthetic mini AST::Module from \p Src that only keeps real
/// bodies for functions in \p BatchSet. Every other defined function
/// body is replaced with `unreachable; end`, which is stack-polymorphic
/// and trivially type-checks against any signature. The funcIdx space
/// is preserved so `call <funcIdx>` instructions in batch bodies still
/// resolve correctly against the original module layout. Non-batch
/// symbols are rewritten as cross-tier thunks by a later pass.
/// Return the AST::FunctionType for module-wide funcIdx \p FuncIdx.
/// Caller must have verified it is in-bounds and promotable.
const AST::FunctionType &getFuncType(const AST::Module &Mod, uint32_t FuncIdx,
                                     uint32_t ImportFuncNum) {
  const uint32_t DefinedIdx = FuncIdx - ImportFuncNum;
  const auto &FuncSec = Mod.getFunctionSection().getContent();
  const uint32_t TypeIdx = FuncSec[DefinedIdx];
  const auto &TypeSec = Mod.getTypeSection().getContent();
  return TypeSec[TypeIdx].getCompositeType().getFuncType();
}

/// Build a synthetic mini AST::Module from \p Src that only keeps real
/// bodies for functions in \p BatchSet. Every other defined function
/// body is replaced with a minimal "push default returns; end" body,
/// which type-checks against the function's own signature. We avoid the
/// obvious `unreachable; end` stub because the LLVM frontend marks the
/// resulting function `noreturn`, and any batch body calling it would
/// be compiled under the assumption the call never returns — collapsing
/// subsequent code paths into unreachable. We post-process the stub
/// bodies of *referenced* non-batch functions in the LLVM module to
/// become tier-2 → tier-1 thunks (see emitT1ThunkInPlace).
AST::Module synthesizeMiniModule(const AST::Module &Src,
                                 const std::unordered_set<uint32_t> &BatchSet,
                                 uint32_t ImportFuncNum) {
  AST::Module Mini(Src);
  auto &CodeSec = Mini.getCodeSection().getContent();
  const auto &FuncSec = Mini.getFunctionSection().getContent();
  const auto &TypeSec = Mini.getTypeSection().getContent();
  for (size_t I = 0; I < CodeSec.size(); ++I) {
    const uint32_t FuncIdx = ImportFuncNum + static_cast<uint32_t>(I);
    if (BatchSet.count(FuncIdx)) {
      continue;
    }
    auto &Seg = CodeSec[I];
    Seg.getLocals().clear();
    auto &Instrs = Seg.getExpr().getInstrs();
    Instrs.clear();
    const uint32_t TypeIdx = FuncSec[I];
    const auto &FT = TypeSec[TypeIdx].getCompositeType().getFuncType();
    for (const auto &RT : FT.getReturnTypes()) {
      switch (RT.getCode()) {
      case TypeCode::I32: {
        AST::Instruction Inst(OpCode::I32__const);
        Inst.setNum(static_cast<uint32_t>(0));
        Instrs.push_back(Inst);
        break;
      }
      case TypeCode::I64: {
        AST::Instruction Inst(OpCode::I64__const);
        Inst.setNum(static_cast<uint64_t>(0));
        Instrs.push_back(Inst);
        break;
      }
      case TypeCode::F32: {
        AST::Instruction Inst(OpCode::F32__const);
        Inst.setNum(0.0f);
        Instrs.push_back(Inst);
        break;
      }
      case TypeCode::F64: {
        AST::Instruction Inst(OpCode::F64__const);
        Inst.setNum(0.0);
        Instrs.push_back(Inst);
        break;
      }
      default:
        // v128/ref — only reachable through non-batch functions that
        // are never called from the batch (because batch filter excludes
        // non-scalar sigs). Fall back to a trapping stub; the function
        // still has a body so validation passes, and LLVM's noreturn
        // inference is harmless for unreferenced functions.
        Instrs.emplace_back(OpCode::Unreachable);
        break;
      }
    }
    Instrs.emplace_back(OpCode::End);
  }
  // The batch bodies came from an already-validated parent module, and
  // the stubbed non-batch bodies validate trivially via stack-polymorphic
  // unreachable. Skip the expensive re-validation — Compiler::compile()
  // only checks the flag.
  Mini.setIsValidated(true);
  return Mini;
}

/// Tier-1's `IR_FASTCALL_FUNC` direct-call convention (x86_64/aarch64 Linux
/// → sysv default) expects a specific return type per wasm ret:
///   void/i32/i64 → i64       (i32 is truncated by the caller)
///   f32          → float
///   f64          → double
LLVMTypeRef tier1ThunkRetType(LLVMContextRef Ctx,
                              Span<const ValType> Rets) noexcept {
  if (Rets.empty()) {
    return LLVMInt64TypeInContext(Ctx);
  }
  switch (Rets.front().getCode()) {
  case TypeCode::F32:
    return LLVMFloatTypeInContext(Ctx);
  case TypeCode::F64:
    return LLVMDoubleTypeInContext(Ctx);
  default:
    return LLVMInt64TypeInContext(Ctx);
  }
}

/// Append `f<FuncIdx>_fwd_thunk` to the LLVM module produced by the
/// WasmEdge LLVM frontend. The thunk is the live-swap entry point we
/// install into tier-1's FuncTable. It:
///   1. calls `wasmedge_tier2_get_exec_ctx` to obtain the thread-local
///      ExecCtxPtrTy value the frontend expects,
///   2. unmarshals each wasm param from the tier-1 `uint64_t *args`
///      scratch area into the correct scalar LLVM type,
///   3. tail-calls `f<FuncIdx>(exec_ctx, params...)` with the frontend's
///      default SysV calling convention,
///   4. remarshals the return into tier-1's expected wire type.
void emitFwdThunk(LLVMModuleRef LLMod, LLVMContextRef Ctx, uint32_t FuncIdx,
                  const AST::FunctionType &FT) {
  const auto &Params = FT.getParamTypes();
  const auto &Rets = FT.getReturnTypes();

  LLVMTypeRef PtrTy = LLVMPointerTypeInContext(Ctx, 0);
  LLVMTypeRef Int64Ty = LLVMInt64TypeInContext(Ctx);
  LLVMTypeRef Int32Ty = LLVMInt32TypeInContext(Ctx);

  // Tier-1 signature: ret (void*, uint64_t*).
  LLVMTypeRef ThunkRetTy = tier1ThunkRetType(Ctx, Rets);
  LLVMTypeRef ThunkParamTys[2] = {PtrTy, PtrTy};
  LLVMTypeRef ThunkFTy =
      LLVMFunctionType(ThunkRetTy, ThunkParamTys, 2, /*IsVarArg=*/0);

  const std::string ThunkName =
      "f" + std::to_string(FuncIdx) + "_fwd_thunk";
  LLVMValueRef Thunk =
      LLVMAddFunction(LLMod, ThunkName.c_str(), ThunkFTy);
  LLVMSetLinkage(Thunk, LLVMExternalLinkage);

  // Match the strictfp+uwtable attributes the LLVM frontend sets on
  // batch functions. Without strictfp on the thunk, LLVM refuses to
  // inline a strictfp callee into a non-strictfp caller — blocking P1c.
  {
    unsigned StrictFPKind = LLVMGetEnumAttributeKindForName(
        "strictfp", static_cast<unsigned>(std::strlen("strictfp")));
    LLVMAddAttributeAtIndex(
        Thunk, LLVMAttributeFunctionIndex,
        LLVMCreateEnumAttribute(Ctx, StrictFPKind, 0));
    unsigned UWTableKind = LLVMGetEnumAttributeKindForName(
        "uwtable", static_cast<unsigned>(std::strlen("uwtable")));
    LLVMAddAttributeAtIndex(
        Thunk, LLVMAttributeFunctionIndex,
        LLVMCreateEnumAttribute(Ctx, UWTableKind, 0));
  }

  // Look up the target function `f<FuncIdx>` already in the module.
  const std::string CalleeName = "f" + std::to_string(FuncIdx);
  LLVMValueRef Callee = LLVMGetNamedFunction(LLMod, CalleeName.c_str());
  if (!Callee) {
    spdlog::error("tier2: fwd_thunk: callee {} not found in module",
                  CalleeName);
    return;
  }
  LLVMTypeRef CalleeFTy = LLVMGlobalGetValueType(Callee);

  LLVMBasicBlockRef Entry = LLVMAppendBasicBlockInContext(Ctx, Thunk, "entry");
  LLVMBuilderRef B = LLVMCreateBuilderInContext(Ctx);
  LLVMPositionBuilderAtEnd(B, Entry);

  // Compute &Executor::ExecutionContext directly from TLS via inline asm.
  // ExecutionContext is a thread_local struct (not a pointer), so we need
  // its address = thread_pointer + offset. Two instructions:
  //   movq %fs:0, %reg    — get thread pointer
  //   addq $OFFSET, %reg  — add fixed offset to get struct address
  // This replaces the previous call to the ORC-bound
  // wasmedge_tier2_get_exec_ctx accessor, eliminating a function-call
  // round-trip per fwd_thunk entry.
  static const ptrdiff_t ExecCtxTlsOff =
      wasmedge_tier2_get_exec_ctx_tls_offset();
  LLVMTypeRef AsmFTy = LLVMFunctionType(PtrTy, nullptr, 0, /*IsVarArg=*/0);
  std::string AsmStr =
      "movq %fs:0, $0\n\taddq $$" + std::to_string(ExecCtxTlsOff) + ", $0";
  std::string AsmCon = "=r";
  LLVMValueRef AsmVal = LLVMGetInlineAsm(
      AsmFTy, AsmStr.c_str(), AsmStr.size(),
      AsmCon.c_str(), AsmCon.size(),
      /*HasSideEffects=*/0, /*IsAlignStack=*/0,
      LLVMInlineAsmDialectATT, /*CanThrow=*/0);
  LLVMValueRef ExecCtx =
      LLVMBuildCall2(B, AsmFTy, AsmVal, nullptr, 0, "exec_ctx");

  // Unmarshal params from args[] scratch buffer.
  LLVMValueRef Args = LLVMGetParam(Thunk, 1);

  // Optional debug trace (env var WASMEDGE_TIER2_TRACE_FUNC).
  if (g_tier2_trace_func >= 0) {
    LLVMTypeRef TraceTys[5] = {LLVMInt32TypeInContext(Ctx), Int64Ty, Int64Ty,
                                Int64Ty, Int64Ty};
    LLVMTypeRef TraceFTy =
        LLVMFunctionType(LLVMVoidTypeInContext(Ctx), TraceTys, 5, 0);
    LLVMValueRef TraceFn =
        LLVMGetNamedFunction(LLMod, "wasmedge_tier2_trace_thunk");
    if (!TraceFn) {
      TraceFn =
          LLVMAddFunction(LLMod, "wasmedge_tier2_trace_thunk", TraceFTy);
      LLVMSetLinkage(TraceFn, LLVMExternalLinkage);
    }
    auto LoadSlot = [&](unsigned I) -> LLVMValueRef {
      LLVMValueRef Idx = LLVMConstInt(Int64Ty, I, 0);
      LLVMValueRef S =
          LLVMBuildInBoundsGEP2(B, Int64Ty, Args, &Idx, 1, "traceslot");
      return LLVMBuildLoad2(B, Int64Ty, S, "trace_raw");
    };
    LLVMValueRef Zero64 = LLVMConstInt(Int64Ty, 0, 0);
    LLVMValueRef TArgs[5] = {
        LLVMConstInt(LLVMInt32TypeInContext(Ctx), FuncIdx, 0),
        Params.size() > 0 ? LoadSlot(0) : Zero64,
        Params.size() > 1 ? LoadSlot(1) : Zero64,
        Params.size() > 2 ? LoadSlot(2) : Zero64,
        Params.size() > 3 ? LoadSlot(3) : Zero64};
    LLVMBuildCall2(B, TraceFTy, TraceFn, TArgs, 5, "");
  }
  std::vector<LLVMValueRef> CallArgs;
  CallArgs.reserve(Params.size() + 1);
  CallArgs.push_back(ExecCtx);
  for (size_t I = 0; I < Params.size(); ++I) {
    LLVMValueRef Idx = LLVMConstInt(Int64Ty, static_cast<uint64_t>(I), 0);
    LLVMValueRef Slot =
        LLVMBuildInBoundsGEP2(B, Int64Ty, Args, &Idx, 1, "slot");
    LLVMValueRef Raw = LLVMBuildLoad2(B, Int64Ty, Slot, "raw");
    LLVMValueRef Casted = nullptr;
    switch (Params[I].getCode()) {
    case TypeCode::I32:
      Casted = LLVMBuildTrunc(B, Raw, Int32Ty, "p");
      break;
    case TypeCode::I64:
      Casted = Raw;
      break;
    case TypeCode::F32: {
      LLVMValueRef Trunc = LLVMBuildTrunc(B, Raw, Int32Ty, "pf32bits");
      Casted = LLVMBuildBitCast(B, Trunc, LLVMFloatTypeInContext(Ctx), "p");
      break;
    }
    case TypeCode::F64:
      Casted = LLVMBuildBitCast(B, Raw, LLVMDoubleTypeInContext(Ctx), "p");
      break;
    default:
      spdlog::error("tier2: fwd_thunk: non-scalar param in f{} — bug in "
                    "promotion filter",
                    FuncIdx);
      LLVMDisposeBuilder(B);
      return;
    }
    CallArgs.push_back(Casted);
  }

  LLVMValueRef CallRes = LLVMBuildCall2(B, CalleeFTy, Callee, CallArgs.data(),
                                        static_cast<unsigned>(CallArgs.size()),
                                        Rets.empty() ? "" : "retv");

  // Remarshal return to tier-1 wire format.
  LLVMValueRef ThunkRet = nullptr;
  if (Rets.empty()) {
    ThunkRet = LLVMConstInt(Int64Ty, 0, 0);
  } else {
    switch (Rets.front().getCode()) {
    case TypeCode::I32:
      ThunkRet = LLVMBuildZExt(B, CallRes, Int64Ty, "retz");
      break;
    case TypeCode::I64:
      ThunkRet = CallRes;
      break;
    case TypeCode::F32:
    case TypeCode::F64:
      ThunkRet = CallRes;
      break;
    default:
      spdlog::error("tier2: fwd_thunk: non-scalar ret in f{} — bug in "
                    "promotion filter",
                    FuncIdx);
      LLVMDisposeBuilder(B);
      return;
    }
  }
  LLVMBuildRet(B, ThunkRet);
  LLVMDisposeBuilder(B);
}

/// Rewrite the stub body of the non-batch function `f<FuncIdx>` already
/// present in \p LLMod (compiled from synthesizeMiniModule's default-value
/// stub) into a tier-2 → tier-1 dispatch thunk. The function's
/// signature stays the SysV `(ExecCtxPtrTy, params...) -> ret` that
/// batch callers reference; we replace its body with:
///   1. allocate uint64_t args[N] on stack
///   2. marshal each param into args[i]
///   3. call `wasmedge_tier2_get_jit_env()` to retrieve the current
///      JitExecEnv* (set by IRJitEngine::invoke)
///   4. load FuncTable[FuncIdx] from env->FuncTable
///   5. invoke target(env, args) via the tier-1 calling convention
///   6. remarshal the return into the frontend's ret type
/// Since we edit the Function's existing Value, every call site in the
/// already-compiled batch bodies that references `@f<FuncIdx>` gets the
/// new body automatically (LLVM references are by Value, not by name).
void emitT1ThunkInPlace(LLVMModuleRef LLMod, LLVMContextRef Ctx,
                        uint32_t FuncIdx, const AST::FunctionType &FT) {
  const std::string Name = "f" + std::to_string(FuncIdx);
  LLVMValueRef Fn = LLVMGetNamedFunction(LLMod, Name.c_str());
  if (!Fn) {
    spdlog::warn("tier2: t1_thunk: target {} missing in module", Name);
    return;
  }

  // Strip attributes that leak compile-time assumptions from the old
  // default-return stub (which may have been marked cold/readnone/etc.
  // during optimization) that would confuse callers now that this body
  // has side effects and dispatches to external code.
  auto Strip = [&](const char *AttrName) {
    unsigned K = LLVMGetEnumAttributeKindForName(
        AttrName, static_cast<unsigned>(std::strlen(AttrName)));
    if (K != 0) {
      LLVMRemoveEnumAttributeAtIndex(Fn, LLVMAttributeFunctionIndex, K);
    }
  };
  for (const char *A :
       {"noreturn", "readnone", "readonly", "memory", "willreturn",
        "mustprogress", "nofree", "norecurse", "cold", "noinline",
        "nounwind"}) {
    Strip(A);
  }

  // Drop existing basic blocks (the compiled default-return stub body).
  while (LLVMBasicBlockRef BB = LLVMGetFirstBasicBlock(Fn)) {
    LLVMDeleteBasicBlock(BB);
  }

  LLVMTypeRef PtrTy = LLVMPointerTypeInContext(Ctx, 0);
  LLVMTypeRef Int64Ty = LLVMInt64TypeInContext(Ctx);
  LLVMTypeRef Int32Ty = LLVMInt32TypeInContext(Ctx);
  LLVMTypeRef FloatTy = LLVMFloatTypeInContext(Ctx);
  LLVMTypeRef DoubleTy = LLVMDoubleTypeInContext(Ctx);

  LLVMBasicBlockRef Entry = LLVMAppendBasicBlockInContext(Ctx, Fn, "entry");
  LLVMBuilderRef B = LLVMCreateBuilderInContext(Ctx);
  LLVMPositionBuilderAtEnd(B, Entry);

  const auto &Params = FT.getParamTypes();
  const auto &Rets = FT.getReturnTypes();

  // Allocate args buffer on stack: [max(1, NumParams) x i64].
  unsigned NumParams = static_cast<unsigned>(Params.size());
  LLVMValueRef ArrLen = LLVMConstInt(Int32Ty, NumParams == 0 ? 1 : NumParams, 0);
  LLVMValueRef ArgsArr = LLVMBuildArrayAlloca(B, Int64Ty, ArrLen, "args");

  // Marshal each param (LLVM param 0 is ExecCtxPtrTy).
  for (unsigned I = 0; I < NumParams; ++I) {
    LLVMValueRef Param = LLVMGetParam(Fn, I + 1);
    LLVMValueRef Raw = nullptr;
    switch (Params[I].getCode()) {
    case TypeCode::I32:
      Raw = LLVMBuildZExt(B, Param, Int64Ty, "zext");
      break;
    case TypeCode::I64:
      Raw = Param;
      break;
    case TypeCode::F32: {
      LLVMValueRef Bits = LLVMBuildBitCast(B, Param, Int32Ty, "f32b");
      Raw = LLVMBuildZExt(B, Bits, Int64Ty, "f32z");
      break;
    }
    case TypeCode::F64:
      Raw = LLVMBuildBitCast(B, Param, Int64Ty, "f64b");
      break;
    default:
      spdlog::error("tier2: t1_thunk: non-scalar param in f{} — bug",
                    FuncIdx);
      LLVMBuildUnreachable(B);
      LLVMDisposeBuilder(B);
      return;
    }
    LLVMValueRef Idx = LLVMConstInt(Int64Ty, I, 0);
    LLVMValueRef Slot =
        LLVMBuildInBoundsGEP2(B, Int64Ty, ArgsArr, &Idx, 1, "aslot");
    LLVMBuildStore(B, Raw, Slot);
  }

  // Load JitExecEnv* directly from TLS via inline asm: movq %fs:OFFSET, %reg.
  // The offset from the thread pointer is fixed once the DSO is loaded and
  // identical across threads, so we compute it once and hardcode it.
  // This replaces the previous call to the ORC-bound wasmedge_tier2_get_jit_env
  // accessor, eliminating a function-call + TLS-accessor round-trip per
  // cross-batch dispatch.
  static const ptrdiff_t TlsOff = wasmedge_tier2_get_jit_env_tls_offset();
  LLVMTypeRef AsmFTy = LLVMFunctionType(PtrTy, nullptr, 0, /*IsVarArg=*/0);
  std::string AsmStr = "movq %fs:" + std::to_string(TlsOff) + ", $0";
  std::string AsmCon = "=r";
  LLVMValueRef AsmVal = LLVMGetInlineAsm(
      AsmFTy, AsmStr.c_str(), AsmStr.size(),
      AsmCon.c_str(), AsmCon.size(),
      /*HasSideEffects=*/0, /*IsAlignStack=*/0,
      LLVMInlineAsmDialectATT, /*CanThrow=*/0);
  LLVMValueRef Env = LLVMBuildCall2(B, AsmFTy, AsmVal, nullptr, 0, "env");

  // FuncTable lives at offset 0 of JitExecEnv (void **FuncTable).
  LLVMValueRef FuncTablePtr =
      LLVMBuildLoad2(B, PtrTy, Env, "func_table");

  // target = FuncTable[FuncIdx]
  LLVMValueRef IdxVal = LLVMConstInt(Int64Ty, FuncIdx, 0);
  LLVMValueRef TgtSlot =
      LLVMBuildInBoundsGEP2(B, PtrTy, FuncTablePtr, &IdxVal, 1, "tslot");
  LLVMValueRef Target = LLVMBuildLoad2(B, PtrTy, TgtSlot, "target");

  // Tier-1 ABI: ret func(JitExecEnv*, uint64_t*)
  LLVMTypeRef Tier1RetTy = tier1ThunkRetType(Ctx, Rets);
  LLVMTypeRef Tier1ParamTys[2] = {PtrTy, PtrTy};
  LLVMTypeRef Tier1FTy =
      LLVMFunctionType(Tier1RetTy, Tier1ParamTys, 2, /*IsVarArg=*/0);

  LLVMValueRef CallArgs[2] = {Env, ArgsArr};
  LLVMValueRef Res = LLVMBuildCall2(B, Tier1FTy, Target, CallArgs, 2,
                                    Rets.empty() ? "" : "tr");

  // Remarshal tier-1 return into the frontend's function-local ret type.
  if (Rets.empty()) {
    LLVMBuildRetVoid(B);
  } else {
    LLVMValueRef Out = nullptr;
    switch (Rets.front().getCode()) {
    case TypeCode::I32:
      // Tier-1 returned i64; frontend expects i32.
      Out = LLVMBuildTrunc(B, Res, Int32Ty, "i32r");
      break;
    case TypeCode::I64:
      Out = Res;
      break;
    case TypeCode::F32:
      // tier1ThunkRetType already returned FloatTy.
      (void)FloatTy;
      Out = Res;
      break;
    case TypeCode::F64:
      (void)DoubleTy;
      Out = Res;
      break;
    default:
      spdlog::error("tier2: t1_thunk: non-scalar ret in f{} — bug",
                    FuncIdx);
      LLVMBuildUnreachable(B);
      LLVMDisposeBuilder(B);
      return;
    }
    LLVMBuildRet(B, Out);
  }
  LLVMDisposeBuilder(B);
}

/// Find the instruction index of the \p LoopIdx-th OSR-eligible Loop in
/// \p Seg's body, matching WasmToIRBuilder::visitLoop's enumeration:
/// increment the counter for a Loop whose enclosing scope stack contains
/// no Loop and no If (intervening Blocks are allowed; the OSR synthesiser
/// rebuilds them). Returns `Instrs.size()` if not found.
uint32_t findOsrLoopStart(const AST::CodeSegment &Seg,
                          uint32_t LoopIdx) noexcept {
  const auto Instrs = Seg.getExpr().getInstrs();
  std::vector<OpCode> Stack;
  uint32_t Count = 0;
  auto enclosedByLoopOrIf = [&]() {
    for (OpCode O : Stack) {
      if (O == OpCode::Loop || O == OpCode::If) {
        return true;
      }
    }
    return false;
  };
  for (size_t I = 0; I < Instrs.size(); ++I) {
    OpCode Op = Instrs[I].getOpCode();
    switch (Op) {
    case OpCode::Loop:
      if (!enclosedByLoopOrIf()) {
        if (Count == LoopIdx)
          return static_cast<uint32_t>(I);
        ++Count;
      }
      Stack.push_back(Op);
      break;
    case OpCode::Block:
    case OpCode::If:
      Stack.push_back(Op);
      break;
    case OpCode::End:
      if (!Stack.empty()) {
        Stack.pop_back();
      }
      break;
    default:
      break;
    }
  }
  return static_cast<uint32_t>(Instrs.size());
}

/// Build a synthetic mini AST::Module for an OSR entry point.
///
/// The target function at \p FuncIdx is rewritten so that:
///   - Its signature is (local0, local1, ..., localN-1) -> original_return,
///     where locals[0..P) are the original parameters followed by
///     declared locals. A brand-new FunctionType is appended to the
///     TypeSection to hold this signature.
///   - Its CodeSegment has zero declared locals (all locals become
///     function parameters) and its instruction body is the tail of the
///     original body starting at the target Loop. Any Block instructions
///     that originally enclosed the target Loop are re-emitted at the
///     front of the OSR body so that `br N` depths inside the Loop still
///     resolve to valid scopes (the scopes' closing `End`s already live
///     in the copied tail).
///
/// Every other defined function in the module gets the usual
/// stack-polymorphic stub body; non-batch callees will be rewritten as
/// tier-2 → tier-1 bridges by emitT1ThunkInPlace() after LLVM codegen.
///
/// Returns true on success, false if the target loop was not found, the
/// enclosing scope chain contains an If (unsupported — would require
/// mid-entering a then/else arm), or validation prerequisites are not met.
bool synthesizeOsrModule(AST::Module &Mini, uint32_t FuncIdx,
                          uint32_t LoopIdx,
                          uint32_t ImportFuncNum) noexcept {
  if (FuncIdx < ImportFuncNum) {
    return false;
  }
  const uint32_t DefinedIdx = FuncIdx - ImportFuncNum;
  auto &CodeSec = Mini.getCodeSection().getContent();
  auto &FuncSec = Mini.getFunctionSection().getContent();
  auto &TypeSec = Mini.getTypeSection().getContent();
  if (DefinedIdx >= CodeSec.size() || DefinedIdx >= FuncSec.size()) {
    return false;
  }

  // Look up the original function type to build the OSR signature.
  const uint32_t OrigTypeIdx = FuncSec[DefinedIdx];
  if (OrigTypeIdx >= TypeSec.size()) {
    return false;
  }
  const auto &OrigFT = TypeSec[OrigTypeIdx].getCompositeType().getFuncType();
  const auto &OrigParams = OrigFT.getParamTypes();
  const auto &OrigRets = OrigFT.getReturnTypes();

  auto &Seg = CodeSec[DefinedIdx];

  // Find the target loop start index.
  const uint32_t LoopStart = findOsrLoopStart(Seg, LoopIdx);
  const auto &Instrs = Seg.getExpr().getInstrs();
  if (LoopStart >= Instrs.size()) {
    return false;
  }
  // The target Loop itself must have an empty BlockType. A typed Loop
  // (loop (param t) ... or loop (result t) ...) expects t-valued stack
  // entry and produces a t-valued stack result — neither is available
  // when the OSR function begins execution right at the Loop header.
  {
    const auto &LT = Instrs[LoopStart].getBlockType();
    if (!LT.isEmpty()) {
      return false;
    }
  }

  // Collect the enclosing Block openers whose matching `End`s live in the
  // tail [LoopStart, Instrs.size()). Walk [0, LoopStart) tracking a stack;
  // what remains on the stack at position LoopStart is the enclosing chain
  // (outermost at index 0). If any entry is an If, bail out — mid-entering
  // an If's then-arm would break wasm's structured control-flow invariants.
  std::vector<size_t> EnclosingOpeners;
  for (size_t I = 0; I < LoopStart; ++I) {
    OpCode Op = Instrs[I].getOpCode();
    if (Op == OpCode::Block || Op == OpCode::Loop || Op == OpCode::If) {
      EnclosingOpeners.push_back(I);
    } else if (Op == OpCode::End) {
      if (!EnclosingOpeners.empty()) {
        EnclosingOpeners.pop_back();
      }
    }
  }
  for (size_t Idx : EnclosingOpeners) {
    OpCode Op = Instrs[Idx].getOpCode();
    if (Op == OpCode::If || Op == OpCode::Loop) {
      // Enclosing If or Loop — cannot synthesize safely.
      return false;
    }
    // Enclosing Block must have empty BlockType. A typed Block demands
    // values on the stack at entry AND at End; neither is available when
    // the OSR function starts execution at the Loop header. (br N targets
    // into such a Block require the expected result values on the operand
    // stack at the br site, so merely rewriting the Block's type to Empty
    // would break wasm validation for those br's.)
    const auto &BT = Instrs[Idx].getBlockType();
    if (!BT.isEmpty()) {
      return false;
    }
  }

  // Build OSR signature params: originalParams ++ declaredLocals (flattened).
  std::vector<ValType> OsrParams;
  OsrParams.reserve(OrigParams.size() + 8);
  for (const auto &P : OrigParams) {
    OsrParams.push_back(P);
  }
  for (const auto &[Cnt, VT] : Seg.getLocals()) {
    for (uint32_t I = 0; I < Cnt; ++I) {
      OsrParams.push_back(VT);
    }
  }

  // Append a new FunctionType for the OSR signature.
  AST::FunctionType OsrFT(OsrParams, OrigRets);
  AST::SubType OsrSub(OsrFT);
  const uint32_t NewTypeIdx = static_cast<uint32_t>(TypeSec.size());
  TypeSec.push_back(std::move(OsrSub));

  // Rewrite the CodeSegment to be the OSR body:
  //   [enclosing Block openers (outermost first)]
  //   [Instrs[LoopStart .. end]]  (includes matching Ends of the openers)
  std::vector<AST::Instruction> OsrBody;
  OsrBody.reserve(EnclosingOpeners.size() + (Instrs.size() - LoopStart));
  for (size_t Idx : EnclosingOpeners) {
    OsrBody.push_back(Instrs[Idx]);
  }
  for (size_t I = LoopStart; I < Instrs.size(); ++I) {
    OsrBody.push_back(Instrs[I]);
  }

  Seg.getLocals().clear();
  Seg.getExpr().getInstrs() = std::move(OsrBody);

  // Repoint this function to the new type index.
  FuncSec[DefinedIdx] = NewTypeIdx;

  // Optional dump for debugging. WASMEDGE_OSR_DUMP=<dir> writes one file
  // per OSR body, named `osr_<funcIdx>_<loopIdx>.txt`, so subsequent
  // crashes don't eat the output before it's flushed.
  if (const char *Dir = std::getenv("WASMEDGE_OSR_DUMP")) {
    std::string Path = std::string(Dir) + "/osr_" +
                       std::to_string(FuncIdx) + "_" +
                       std::to_string(LoopIdx) + ".txt";
    if (FILE *F = std::fopen(Path.c_str(), "w")) {
      const auto &Body = Seg.getExpr().getInstrs();
      std::fprintf(F, "func=%u loop=%u body_size=%zu rets=%zu params=%zu\n",
                   FuncIdx, LoopIdx, Body.size(), OrigRets.size(),
                   OsrParams.size());
      int Depth = 0;
      for (size_t I = 0; I < Body.size(); ++I) {
        OpCode Op = Body[I].getOpCode();
        if (Op == OpCode::End)
          Depth = std::max(0, Depth - 1);
        std::fprintf(F, "  [%5zu] ", I);
        for (int D = 0; D < Depth; ++D)
          std::fputs("  ", F);
        auto OpName = OpCodeStr[Op];
        std::fprintf(F, "%.*s", static_cast<int>(OpName.size()),
                     OpName.data());
        if (Op == OpCode::Block || Op == OpCode::Loop || Op == OpCode::If) {
          const auto &BT = Body[I].getBlockType();
          if (BT.isEmpty()) {
            std::fputs(" [empty]", F);
          } else if (BT.isValType()) {
            std::fputs(" [valtype]", F);
          } else {
            std::fprintf(F, " [typeidx=%u]", BT.getTypeIndex());
          }
        } else if (Op == OpCode::Br || Op == OpCode::Br_if) {
          std::fprintf(F, " depth=%u", Body[I].getJump().TargetIndex);
        }
        std::fputc('\n', F);
        if (Op == OpCode::Block || Op == OpCode::Loop || Op == OpCode::If)
          ++Depth;
      }
      std::fclose(F);
    }
  }

  return true;
}

/// Walk the bodies of every function in \p BatchSet and collect all direct
/// `call` targets that are (a) defined (not imports) and (b) not already
/// in the batch. These are the non-batch functions for which we must
/// emit tier-2 → tier-1 bridges.
/// Scalar-only MVP promotion filter, mirroring the one used by the batch
/// planner in Tier2Manager (see lib/vm/tier2_manager.cpp). Duplicated here
/// because the compiler must make the same decision when expanding the OSR
/// batch, and there is no cross-TU header for the manager's private helpers.
/// Rejects: imports, multi-return functions, non-scalar param/ret types,
/// and trap-stub bodies that start with `unreachable` (the frontend skips
/// emitting `f<Idx>` for those, which would break emitFwdThunk lookup).
bool isOsrBatchPromotable(const AST::Module &Mod, uint32_t FuncIdx,
                           uint32_t ImportFuncNum) noexcept {
  if (FuncIdx < ImportFuncNum) {
    return false;
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
    return false;
  }
  auto Marshalable = [](const ValType &VT) {
    switch (VT.getCode()) {
    case TypeCode::I32:
    case TypeCode::I64:
    case TypeCode::F32:
    case TypeCode::F64:
      return true;
    default:
      return false;
    }
  };
  for (const auto &PT : FT.getParamTypes()) {
    if (!Marshalable(PT))
      return false;
  }
  for (const auto &RT : FT.getReturnTypes()) {
    if (!Marshalable(RT))
      return false;
  }
  return true;
}

/// BFS over direct `call` edges in \p Mod starting from \p Root, collecting
/// promotable defined-function indices up to depth \p MaxDepth and cap
/// \p MaxSize. Root is always included regardless of promotability (caller
/// vets the root; for OSR, emitFwdThunk rejects non-scalar params at emit
/// time and we surface that as a warn+bail). The returned set is used as
/// the OSR batch: its members keep real bodies in the synthesized mini
/// module, get marked internal+alwaysinline so O2 inlines helper calls
/// into the OSR body, and are excluded from the t1_thunk rewrite pass.
std::unordered_set<uint32_t>
bfsOsrBatch(const AST::Module &Mod, uint32_t Root, uint32_t ImportFuncNum,
             uint32_t MaxDepth, uint32_t MaxSize) noexcept {
  std::unordered_set<uint32_t> Batch;
  Batch.insert(Root);
  const auto &CodeSec = Mod.getCodeSection().getContent();
  std::deque<std::pair<uint32_t, uint32_t>> Frontier;
  Frontier.push_back({Root, 0});
  while (!Frontier.empty() && Batch.size() < MaxSize) {
    auto [F, D] = Frontier.front();
    Frontier.pop_front();
    if (D >= MaxDepth) {
      continue;
    }
    if (F < ImportFuncNum) {
      continue;
    }
    const uint32_t DefinedIdx = F - ImportFuncNum;
    if (DefinedIdx >= CodeSec.size()) {
      continue;
    }
    const auto Instrs = CodeSec[DefinedIdx].getExpr().getInstrs();
    std::unordered_set<uint32_t> LocalSeen;
    for (auto It = Instrs.begin(); It != Instrs.end(); ++It) {
      if (It->getOpCode() != OpCode::Call) {
        continue;
      }
      const uint32_t Tgt = It->getTargetIndex();
      if (Tgt < ImportFuncNum) {
        continue;
      }
      if (Batch.count(Tgt)) {
        continue;
      }
      if (!LocalSeen.insert(Tgt).second) {
        continue;
      }
      if (!isOsrBatchPromotable(Mod, Tgt, ImportFuncNum)) {
        continue;
      }
      if (Batch.size() >= MaxSize) {
        break;
      }
      Batch.insert(Tgt);
      Frontier.push_back({Tgt, D + 1});
    }
  }
  return Batch;
}

std::vector<uint32_t>
collectNonBatchCallees(const AST::Module &Mod,
                       const std::unordered_set<uint32_t> &BatchSet,
                       uint32_t ImportFuncNum) noexcept {
  std::vector<uint32_t> Result;
  std::unordered_set<uint32_t> Seen;
  const auto &CodeSec = Mod.getCodeSection().getContent();
  for (uint32_t FuncIdx : BatchSet) {
    if (FuncIdx < ImportFuncNum) {
      continue;
    }
    const uint32_t DefinedIdx = FuncIdx - ImportFuncNum;
    if (DefinedIdx >= CodeSec.size()) {
      continue;
    }
    const auto Instrs = CodeSec[DefinedIdx].getExpr().getInstrs();
    for (auto It = Instrs.begin(); It != Instrs.end(); ++It) {
      if (It->getOpCode() != OpCode::Call) {
        continue;
      }
      uint32_t Target = It->getTargetIndex();
      if (Target < ImportFuncNum) {
        continue; // imports are handled by the frontend's intrinsics path
      }
      if (BatchSet.count(Target)) {
        continue;
      }
      if (Seen.insert(Target).second) {
        Result.push_back(Target);
      }
    }
  }
  return Result;
}

} // namespace

struct Tier2Compiler::Impl {
  LLVMTargetMachineRef TM = nullptr;
  char *Triple = nullptr;

  /// ORC LLJITs produced by compileBatch() are retained here for the
  /// lifetime of the Tier2Compiler, so FuncTable entries returned to the
  /// worker stay valid. We never de-tier, so liveness == compiler lifetime.
  std::vector<LLVMOrcLLJITRef> JITs;

  ~Impl() {
    for (auto *JIT : JITs) {
      if (JIT) {
        LLVMOrcDisposeLLJIT(JIT);
      }
    }
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

  if (const char *E = ::getenv("WASMEDGE_TIER2_TRACE_FUNC")) {
    g_tier2_trace_func = std::atoi(E);
  }

  P->Triple = LLVMGetDefaultTargetTriple();
  LLVMTargetRef Target = nullptr;
  char *Err = nullptr;
  if (!LLVMGetTargetFromTriple(P->Triple, &Target, &Err)) {
    P->TM = LLVMCreateTargetMachine(Target, P->Triple, "generic", "",
                                    LLVMCodeGenLevelDefault, LLVMRelocPIC,
                                    LLVMCodeModelJITDefault);
  } else {
    spdlog::error("tier2: failed to get target for {}: {}", P->Triple,
                  Err ? Err : "(null)");
    LLVMDisposeMessage(Err);
  }
}

Tier2Compiler::~Tier2Compiler() noexcept = default;

Expect<std::vector<std::pair<uint32_t, void *>>>
Tier2Compiler::compileBatch(Span<const uint32_t> BatchIdx,
                            const AST::Module &Mod, unsigned OptLevel) {
  std::vector<std::pair<uint32_t, void *>> Result;
  if (isShutdown() || BatchIdx.empty()) {
    return Result;
  }

  // Derive import func count from the full module.
  uint32_t ImportFuncNum = 0;
  for (const auto &ImpDesc : Mod.getImportSection().getContent()) {
    if (ImpDesc.getExternalType() == ExternalType::Function) {
      ++ImportFuncNum;
    }
  }

  std::unordered_set<uint32_t> BatchSet;
  BatchSet.reserve(BatchIdx.size());
  for (uint32_t F : BatchIdx) {
    BatchSet.insert(F);
  }

  AST::Module Mini = synthesizeMiniModule(Mod, BatchSet, ImportFuncNum);

  if (isShutdown()) {
    return Result;
  }

  // Build a Configure tuned for tier-2 JIT: gas / instr counting / cost
  // measuring off so the runtime ExecCtx nullables stay safe, and the
  // WasmEdge LLVM frontend doesn't emit interrupt-check code.
  //
  // IMPORTANT: force O0 for the frontend compile. The LLVM opt pipeline
  // would otherwise inline / DCE the constant-return bodies we use as
  // stubs for non-batch defined functions *before* we get a chance to
  // rewrite those bodies into t1_thunks. Callers would then hold onto a
  // folded constant instead of the real cross-tier bridge. We run our
  // own opt pipeline after post-processing.
  Configure Conf;
  Conf.getCompilerConfigure().setOptimizationLevel(
      CompilerConfigure::OptimizationLevel::O0);
  Conf.getCompilerConfigure().setInterruptible(false);
  Conf.getCompilerConfigure().setGenericBinary(false);
  Conf.getStatisticsConfigure().setInstructionCounting(false);
  Conf.getStatisticsConfigure().setCostMeasuring(false);
  Conf.getStatisticsConfigure().setTimeMeasuring(false);

  LLVM::Compiler LC(Conf);
  auto CompRes = LC.compile(Mini);
  if (!CompRes) {
    spdlog::warn("tier2: LLVM frontend compile failed (head func {})",
                 BatchIdx.front());
    return Result;
  }

  LLVMModuleRef LLMod = CompRes->getModuleRef();
  LLVMContextRef LLCtx = CompRes->getContextRef();
  if (!LLMod || !LLCtx) {
    spdlog::error("tier2: compiled Data has no LLVM module/context");
    return Result;
  }

  for (uint32_t FuncIdx : BatchIdx) {
    const auto &FT = getFuncType(Mod, FuncIdx, ImportFuncNum);
    emitFwdThunk(LLMod, LLCtx, FuncIdx, FT);
  }

  // Replace the stub body of every non-batch defined function that is
  // referenced by a batch body with a tier-2 → tier-1 dispatch thunk.
  // This is what makes cross-batch calls safe: without this pass the
  // batch would execute a `[const, end]` stub and silently drop the
  // call, corrupting the caller's logical state.
  const auto NonBatchCallees =
      collectNonBatchCallees(Mod, BatchSet, ImportFuncNum);
  for (uint32_t CalleeIdx : NonBatchCallees) {
    const auto &FT = getFuncType(Mod, CalleeIdx, ImportFuncNum);
    emitT1ThunkInPlace(LLMod, LLCtx, CalleeIdx, FT);
  }
  if (!NonBatchCallees.empty()) {
    spdlog::debug("tier2: rewrote {} non-batch stubs as t1_thunks",
                  NonBatchCallees.size());
  }

  // P1c: Demote batch functions from `protected dllexport external` (the
  // frontend default) to internal. That alone unblocks LLVM's inliner:
  // the fwd_thunk calls `f<N>` from exactly one site, so the single-
  // callsite bonus will fold the body in regardless of size. Cross-body
  // calls between batch members are then judged by the normal cost model
  // — which correctly declines to inline e.g. a 26k-line f8 at 805 call
  // sites of __multi3, the explosion we hit under `alwaysinline`.
  for (uint32_t FuncIdx : BatchIdx) {
    std::string Name = "f" + std::to_string(FuncIdx);
    LLVMValueRef Fn = LLVMGetNamedFunction(LLMod, Name.c_str());
    if (!Fn)
      continue;
    LLVMSetLinkage(Fn, LLVMInternalLinkage);
    LLVMSetVisibility(Fn, LLVMDefaultVisibility);
    LLVMSetDLLStorageClass(Fn, LLVMDefaultStorageClass);
  }

  // Run LLVM opt on the post-processed module now that all thunks and
  // stub rewrites are in place. The WasmEdge frontend was invoked at O0,
  // so call sites to non-batch stubs (now t1_thunks) are still intact.
  // Opt here is free to inline t1_thunks where profitable, and batch
  // functions (now alwaysinline) get merged into their fwd_thunks.
  if (P->TM && OptLevel > 0) {
    const char *Pipeline = OptLevel >= 3 ? "default<O3>"
                            : OptLevel == 2 ? "default<O2>"
                                             : "default<O1>";
    LLVMPassBuilderOptionsRef PBO = LLVMCreatePassBuilderOptions();
    if (LLVMErrorRef Err = LLVMRunPasses(LLMod, Pipeline, P->TM, PBO)) {
      char *Msg = LLVMGetErrorMessage(Err);
      spdlog::warn("tier2: post-opt pipeline failed: {}",
                   Msg ? Msg : "(null)");
      LLVMDisposeErrorMessage(Msg);
    }
    LLVMDisposePassBuilderOptions(PBO);
  }

  if (const char *DumpDir = ::getenv("WASMEDGE_TIER2_DUMP_IR")) {
    std::string Path = std::string(DumpDir);
    if (!Path.empty() && Path.back() != '/') {
      Path.push_back('/');
    }
    Path += "tier2_f" + std::to_string(BatchIdx.front()) + ".ll";
    char *Err = nullptr;
    if (LLVMPrintModuleToFile(LLMod, Path.c_str(), &Err)) {
      spdlog::warn("tier2: dump module to {} failed: {}", Path,
                   Err ? Err : "(null)");
      LLVMDisposeMessage(Err);
    } else {
      spdlog::info("tier2: dumped module to {}", Path);
    }
  }

  // Take ownership of the module + its thread-safe context, then hand
  // them to a fresh ORC LLJIT.
  LLVMModuleRef OwnedMod = CompRes->releaseModule();
  LLVMOrcThreadSafeContextRef TSCtx = CompRes->releaseTSContext();
  if (!OwnedMod || !TSCtx) {
    spdlog::error("tier2: failed to release module/TSContext");
    if (OwnedMod)
      LLVMDisposeModule(OwnedMod);
    if (TSCtx)
      LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Result;
  }

  LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
  LLVMOrcLLJITRef JIT = nullptr;
  if (LLVMErrorRef Err = LLVMOrcCreateLLJIT(&JIT, Builder)) {
    char *Msg = LLVMGetErrorMessage(Err);
    spdlog::error("tier2: LLVMOrcCreateLLJIT failed: {}",
                  Msg ? Msg : "(null)");
    LLVMDisposeErrorMessage(Msg);
    LLVMDisposeModule(OwnedMod);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Result;
  }

  LLVMOrcJITDylibRef MainJD = LLVMOrcLLJITGetMainJITDylib(JIT);
  LLVMOrcExecutionSessionRef ES = LLVMOrcLLJITGetExecutionSession(JIT);

  // Define tier-2 helpers as absolute symbols:
  //   - wasmedge_tier2_trace_thunk: debug trace for thunk entry.
  // Note: wasmedge_tier2_get_jit_env and wasmedge_tier2_get_exec_ctx were
  // removed — both t1_thunks and fwd_thunks now compute their TLS values
  // directly via inline asm (%fs:OFFSET).
  {
    LLVMOrcCSymbolMapPair Pairs[1];
    Pairs[0].Name =
        LLVMOrcExecutionSessionIntern(ES, "wasmedge_tier2_trace_thunk");
    Pairs[0].Sym.Address = reinterpret_cast<LLVMOrcJITTargetAddress>(
        reinterpret_cast<void *>(&wasmedge_tier2_trace_thunk));
    Pairs[0].Sym.Flags.GenericFlags =
        LLVMJITSymbolGenericFlagsExported | LLVMJITSymbolGenericFlagsCallable;
    Pairs[0].Sym.Flags.TargetFlags = 0;

    LLVMOrcMaterializationUnitRef MU = LLVMOrcAbsoluteSymbols(Pairs, 1);
    if (LLVMErrorRef Err = LLVMOrcJITDylibDefine(MainJD, MU)) {
      char *Msg = LLVMGetErrorMessage(Err);
      spdlog::error("tier2: define absolute symbols failed: {}",
                    Msg ? Msg : "(null)");
      LLVMDisposeErrorMessage(Msg);
      LLVMOrcDisposeLLJIT(JIT);
      LLVMDisposeModule(OwnedMod);
      LLVMOrcDisposeThreadSafeContext(TSCtx);
      return Result;
    }
  }

  // Hand the module to ORC. addLLVMIRModule takes ownership of the
  // ThreadSafeModule (which shares ownership of TSCtx). We still own our
  // TSCtx handle and dispose it at the end.
  LLVMOrcThreadSafeModuleRef TSM =
      LLVMOrcCreateNewThreadSafeModule(OwnedMod, TSCtx);
  OwnedMod = nullptr; // TSM owns the module now.
  if (LLVMErrorRef Err = LLVMOrcLLJITAddLLVMIRModule(JIT, MainJD, TSM)) {
    char *Msg = LLVMGetErrorMessage(Err);
    spdlog::error("tier2: addLLVMIRModule failed: {}", Msg ? Msg : "(null)");
    LLVMDisposeErrorMessage(Msg);
    LLVMOrcDisposeLLJIT(JIT);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Result;
  }

  // Look up `intrinsics` global and install &Executor::Intrinsics. The
  // LLVM frontend emits `intrinsics` as a mutable external global (see
  // lib/llvm/compiler.cpp:5981 — set null initializer, globalConstant=false).
  // At runtime the loader writes the real intrinsics table address into
  // the global's storage. Tier-2 must do the same.
  {
    LLVMOrcJITTargetAddress Addr = 0;
    if (LLVMErrorRef Err = LLVMOrcLLJITLookup(JIT, &Addr, "intrinsics")) {
      char *Msg = LLVMGetErrorMessage(Err);
      spdlog::error("tier2: lookup `intrinsics` failed: {}",
                    Msg ? Msg : "(null)");
      LLVMDisposeErrorMessage(Msg);
      LLVMOrcDisposeLLJIT(JIT);
      LLVMOrcDisposeThreadSafeContext(TSCtx);
      return Result;
    }
    auto **Slot = reinterpret_cast<const Executable::IntrinsicsTable **>(
        static_cast<uintptr_t>(Addr));
    *Slot = &Executor::Executor::Intrinsics;
  }

  // Look up each batch fwd_thunk address for atomic FuncTable swap.
  Result.reserve(BatchIdx.size());
  for (uint32_t FuncIdx : BatchIdx) {
    const std::string ThunkName =
        "f" + std::to_string(FuncIdx) + "_fwd_thunk";
    LLVMOrcJITTargetAddress Addr = 0;
    if (LLVMErrorRef Err =
            LLVMOrcLLJITLookup(JIT, &Addr, ThunkName.c_str())) {
      char *Msg = LLVMGetErrorMessage(Err);
      spdlog::warn("tier2: lookup {} failed: {}", ThunkName,
                   Msg ? Msg : "(null)");
      LLVMDisposeErrorMessage(Msg);
      continue;
    }
    Result.emplace_back(FuncIdx,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(Addr)));
  }

  // Retain the JIT for the lifetime of this Tier2Compiler — the code
  // returned to the FuncTable swap lives inside it.
  P->JITs.push_back(JIT);
  LLVMOrcDisposeThreadSafeContext(TSCtx);

  spdlog::info("tier2: step-5 ORC done for func {} (emitted {} thunks)",
               BatchIdx.front(), Result.size());
  return Result;
}

Expect<void *>
Tier2Compiler::compileOsrEntry(uint32_t FuncIdx, uint32_t LoopIdx,
                                const AST::Module &Mod, unsigned OptLevel) {
  if (isShutdown()) {
    return nullptr;
  }

  uint32_t ImportFuncNum = 0;
  for (const auto &ImpDesc : Mod.getImportSection().getContent()) {
    if (ImpDesc.getExternalType() == ExternalType::Function) {
      ++ImportFuncNum;
    }
  }
  if (FuncIdx < ImportFuncNum) {
    spdlog::warn("tier2-osr: func {} is an import; cannot OSR", FuncIdx);
    return nullptr;
  }

  // Start from a copy of the module so we can surgically rewrite the OSR
  // function without mutating the shared parsed module.
  AST::Module Mini(Mod);

  // Rewrite the target function into its OSR form (locals-as-params,
  // body = [loop..end]). Must be done before stubbing non-OSR bodies so
  // synthesizeMiniModule doesn't zero the Instrs vector we care about.
  if (!synthesizeOsrModule(Mini, FuncIdx, LoopIdx, ImportFuncNum)) {
    spdlog::warn("tier2-osr: synth failed for func {} loop {}", FuncIdx,
                 LoopIdx);
    return nullptr;
  }

  // Expand the batch BFS-style from the OSR function: pull in its direct
  // (and, up to MaxDepth, transitive) promotable callees so their real
  // bodies stay in the mini-module. Without this the OSR function is a
  // singleton and every helper call becomes an indirect FuncTable
  // dispatch through a t1_thunk — LLVM can't see past the function
  // pointer, so tight helpers (char-class predicates in shootout-ctype,
  // 25519 field-arithmetic in shootout-ed25519) never get inlined into
  // the hot loop. Mirrors the regular batch path's inlining recipe.
  constexpr uint32_t OsrBatchMaxDepth = 2;
  constexpr uint32_t OsrBatchMaxSize = 12;
  std::unordered_set<uint32_t> BatchSet =
      bfsOsrBatch(Mini, FuncIdx, ImportFuncNum, OsrBatchMaxDepth,
                   OsrBatchMaxSize);
  if (BatchSet.size() > 1) {
    spdlog::info("tier2-osr: batch expanded to {} funcs (root {})",
                 BatchSet.size(), FuncIdx);
  }
  {
    auto &CodeSec = Mini.getCodeSection().getContent();
    const auto &FuncSec = Mini.getFunctionSection().getContent();
    const auto &TypeSec = Mini.getTypeSection().getContent();
    for (size_t I = 0; I < CodeSec.size(); ++I) {
      const uint32_t Idx = ImportFuncNum + static_cast<uint32_t>(I);
      if (BatchSet.count(Idx))
        continue;
      auto &Seg = CodeSec[I];
      Seg.getLocals().clear();
      auto &Instrs = Seg.getExpr().getInstrs();
      Instrs.clear();
      const uint32_t TypeIdx = FuncSec[I];
      const auto &FT = TypeSec[TypeIdx].getCompositeType().getFuncType();
      for (const auto &RT : FT.getReturnTypes()) {
        switch (RT.getCode()) {
        case TypeCode::I32: {
          AST::Instruction Inst(OpCode::I32__const);
          Inst.setNum(static_cast<uint32_t>(0));
          Instrs.push_back(Inst);
          break;
        }
        case TypeCode::I64: {
          AST::Instruction Inst(OpCode::I64__const);
          Inst.setNum(static_cast<uint64_t>(0));
          Instrs.push_back(Inst);
          break;
        }
        case TypeCode::F32: {
          AST::Instruction Inst(OpCode::F32__const);
          Inst.setNum(0.0f);
          Instrs.push_back(Inst);
          break;
        }
        case TypeCode::F64: {
          AST::Instruction Inst(OpCode::F64__const);
          Inst.setNum(0.0);
          Instrs.push_back(Inst);
          break;
        }
        default:
          Instrs.emplace_back(OpCode::Unreachable);
          break;
        }
      }
      Instrs.emplace_back(OpCode::End);
    }
  }
  // Unlike the regular batch path, the OSR body is a genuine rewrite of
  // the function body (locals flattened to params, enclosing Block chain
  // synthesized). The stubbed non-OSR bodies validate trivially, but the
  // OSR body itself could fail validation if its operand stack balance
  // diverges from the original (e.g. the original pushed values before
  // the Loop that get popped after it — OSR can't reconstruct those).
  // Run the validator so we catch such cases as a clean reject rather
  // than an LLVM-backend assertion crash.
  {
    Configure ValConf;
    Validator::Validator V(ValConf);
    if (auto VR = V.validate(Mini); !VR) {
      spdlog::warn(
          "tier2-osr: validator rejected synthesized module "
          "(func {} loop {}): {}",
          FuncIdx, LoopIdx, ErrCodeStr[VR.error().getEnum()]);
      return nullptr;
    }
  }
  Mini.setIsValidated(true);

  if (isShutdown()) {
    return nullptr;
  }

  // Compile the synthesized module.
  Configure Conf;
  Conf.getCompilerConfigure().setOptimizationLevel(
      CompilerConfigure::OptimizationLevel::O0);
  Conf.getCompilerConfigure().setInterruptible(false);
  Conf.getCompilerConfigure().setGenericBinary(false);
  Conf.getStatisticsConfigure().setInstructionCounting(false);
  Conf.getStatisticsConfigure().setCostMeasuring(false);
  Conf.getStatisticsConfigure().setTimeMeasuring(false);

  LLVM::Compiler LC(Conf);
  auto CompRes = LC.compile(Mini);
  if (!CompRes) {
    spdlog::warn("tier2-osr: frontend compile failed (func {} loop {})",
                 FuncIdx, LoopIdx);
    return nullptr;
  }
  LLVMModuleRef LLMod = CompRes->getModuleRef();
  LLVMContextRef LLCtx = CompRes->getContextRef();
  if (!LLMod || !LLCtx) {
    spdlog::error("tier2-osr: compiled Data has no LLVM module/context");
    return nullptr;
  }

  // Build the OSR function type (must reflect the rewritten signature:
  // (all locals as params) -> original return). Read from Mini since
  // that's the source of truth for the new type index.
  const uint32_t DefinedIdx = FuncIdx - ImportFuncNum;
  const uint32_t NewTypeIdx =
      Mini.getFunctionSection().getContent()[DefinedIdx];
  const auto &OsrFT =
      Mini.getTypeSection().getContent()[NewTypeIdx].getCompositeType()
          .getFuncType();

  // Reuse emitFwdThunk: it generates a tier-1 ABI entry
  // `f<FuncIdx>_fwd_thunk(JitExecEnv*, uint64_t* args)` that unmarshals
  // args[] into the LLVM function's native param types. Because the OSR
  // function's params are all wasm locals and the OsrLocalsFrame is laid
  // out exactly the same way, the locals buffer passed by tier-1 can be
  // treated as an args[] buffer for ABI purposes.
  emitFwdThunk(LLMod, LLCtx, FuncIdx, OsrFT);

  // Rewrite every non-OSR defined function referenced by the OSR body
  // as a tier-2 → tier-1 dispatch bridge. This is the OSR equivalent of
  // t1_thunks in a regular batch.
  const auto NonBatchCallees =
      collectNonBatchCallees(Mini, BatchSet, ImportFuncNum);
  for (uint32_t CalleeIdx : NonBatchCallees) {
    // Use the ORIGINAL module's FuncType — Mini's stubbed bodies keep
    // the original signature, and t1_thunks dispatch by funcIdx at
    // runtime against the tier-1 FuncTable which uses the original types.
    const auto &FT = getFuncType(Mod, CalleeIdx, ImportFuncNum);
    emitT1ThunkInPlace(LLMod, LLCtx, CalleeIdx, FT);
  }
  if (!NonBatchCallees.empty()) {
    spdlog::debug("tier2-osr: rewrote {} non-batch stubs as t1_thunks",
                  NonBatchCallees.size());
  }

  // Demote batch members to internal linkage so O2's inliner is allowed
  // to fold them. The OSR fwd_thunk's single call to `f<FuncIdx>` is
  // inlined by the single-callsite bonus; cross-body inlines inside the
  // batch are judged by the cost model. Using `alwaysinline` here is
  // what caused the ed25519 blow-up (805× __multi3 + 60× fe25519_mul
  // unconditionally inlined into f8's 26k-line body). Callees outside
  // the batch remain reached via t1_thunks (FuncTable dispatch) —
  // opaque to LLVM and left as indirect calls.
  for (uint32_t BIdx : BatchSet) {
    std::string Name = "f" + std::to_string(BIdx);
    LLVMValueRef Fn = LLVMGetNamedFunction(LLMod, Name.c_str());
    if (!Fn) {
      continue;
    }
    LLVMSetLinkage(Fn, LLVMInternalLinkage);
    LLVMSetVisibility(Fn, LLVMDefaultVisibility);
    LLVMSetDLLStorageClass(Fn, LLVMDefaultStorageClass);
  }

  if (P->TM && OptLevel > 0) {
    const char *Pipeline = OptLevel >= 3 ? "default<O3>"
                            : OptLevel == 2 ? "default<O2>"
                                             : "default<O1>";
    LLVMPassBuilderOptionsRef PBO = LLVMCreatePassBuilderOptions();
    if (LLVMErrorRef Err = LLVMRunPasses(LLMod, Pipeline, P->TM, PBO)) {
      char *Msg = LLVMGetErrorMessage(Err);
      spdlog::warn("tier2-osr: post-opt pipeline failed: {}",
                   Msg ? Msg : "(null)");
      LLVMDisposeErrorMessage(Msg);
    }
    LLVMDisposePassBuilderOptions(PBO);
  }

  if (const char *DumpDir = ::getenv("WASMEDGE_TIER2_DUMP_IR")) {
    std::string Path = std::string(DumpDir);
    if (!Path.empty() && Path.back() != '/') {
      Path.push_back('/');
    }
    Path += "tier2_osr_f" + std::to_string(FuncIdx) + "_l" +
            std::to_string(LoopIdx) + ".ll";
    char *Err = nullptr;
    if (LLVMPrintModuleToFile(LLMod, Path.c_str(), &Err)) {
      spdlog::warn("tier2-osr: dump module to {} failed: {}", Path,
                   Err ? Err : "(null)");
      LLVMDisposeMessage(Err);
    } else {
      spdlog::info("tier2-osr: dumped module to {}", Path);
    }
  }

  LLVMModuleRef OwnedMod = CompRes->releaseModule();
  LLVMOrcThreadSafeContextRef TSCtx = CompRes->releaseTSContext();
  if (!OwnedMod || !TSCtx) {
    spdlog::error("tier2-osr: failed to release module/TSContext");
    if (OwnedMod)
      LLVMDisposeModule(OwnedMod);
    if (TSCtx)
      LLVMOrcDisposeThreadSafeContext(TSCtx);
    return nullptr;
  }

  LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
  LLVMOrcLLJITRef JIT = nullptr;
  if (LLVMErrorRef Err = LLVMOrcCreateLLJIT(&JIT, Builder)) {
    char *Msg = LLVMGetErrorMessage(Err);
    spdlog::error("tier2-osr: LLVMOrcCreateLLJIT failed: {}",
                  Msg ? Msg : "(null)");
    LLVMDisposeErrorMessage(Msg);
    LLVMDisposeModule(OwnedMod);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return nullptr;
  }

  LLVMOrcJITDylibRef MainJD = LLVMOrcLLJITGetMainJITDylib(JIT);
  LLVMOrcExecutionSessionRef ES = LLVMOrcLLJITGetExecutionSession(JIT);
  {
    LLVMOrcCSymbolMapPair Pairs[1];
    Pairs[0].Name =
        LLVMOrcExecutionSessionIntern(ES, "wasmedge_tier2_trace_thunk");
    Pairs[0].Sym.Address = reinterpret_cast<LLVMOrcJITTargetAddress>(
        reinterpret_cast<void *>(&wasmedge_tier2_trace_thunk));
    Pairs[0].Sym.Flags.GenericFlags =
        LLVMJITSymbolGenericFlagsExported | LLVMJITSymbolGenericFlagsCallable;
    Pairs[0].Sym.Flags.TargetFlags = 0;
    LLVMOrcMaterializationUnitRef MU = LLVMOrcAbsoluteSymbols(Pairs, 1);
    if (LLVMErrorRef Err = LLVMOrcJITDylibDefine(MainJD, MU)) {
      char *Msg = LLVMGetErrorMessage(Err);
      spdlog::error("tier2-osr: define absolute symbols failed: {}",
                    Msg ? Msg : "(null)");
      LLVMDisposeErrorMessage(Msg);
      LLVMOrcDisposeLLJIT(JIT);
      LLVMDisposeModule(OwnedMod);
      LLVMOrcDisposeThreadSafeContext(TSCtx);
      return nullptr;
    }
  }

  LLVMOrcThreadSafeModuleRef TSM =
      LLVMOrcCreateNewThreadSafeModule(OwnedMod, TSCtx);
  OwnedMod = nullptr;
  if (LLVMErrorRef Err = LLVMOrcLLJITAddLLVMIRModule(JIT, MainJD, TSM)) {
    char *Msg = LLVMGetErrorMessage(Err);
    spdlog::error("tier2-osr: addLLVMIRModule failed: {}",
                  Msg ? Msg : "(null)");
    LLVMDisposeErrorMessage(Msg);
    LLVMOrcDisposeLLJIT(JIT);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return nullptr;
  }

  // Install intrinsics table.
  {
    LLVMOrcJITTargetAddress Addr = 0;
    if (LLVMErrorRef Err = LLVMOrcLLJITLookup(JIT, &Addr, "intrinsics")) {
      char *Msg = LLVMGetErrorMessage(Err);
      spdlog::error("tier2-osr: lookup `intrinsics` failed: {}",
                    Msg ? Msg : "(null)");
      LLVMDisposeErrorMessage(Msg);
      LLVMOrcDisposeLLJIT(JIT);
      LLVMOrcDisposeThreadSafeContext(TSCtx);
      return nullptr;
    }
    auto **Slot = reinterpret_cast<const Executable::IntrinsicsTable **>(
        static_cast<uintptr_t>(Addr));
    *Slot = &Executor::Executor::Intrinsics;
  }

  // Look up the OSR entry address (same `f<FuncIdx>_fwd_thunk` name the
  // batch path uses — OSR reuses the tier-1 ABI).
  const std::string ThunkName = "f" + std::to_string(FuncIdx) + "_fwd_thunk";
  LLVMOrcJITTargetAddress Addr = 0;
  if (LLVMErrorRef Err = LLVMOrcLLJITLookup(JIT, &Addr, ThunkName.c_str())) {
    char *Msg = LLVMGetErrorMessage(Err);
    spdlog::warn("tier2-osr: lookup {} failed: {}", ThunkName,
                 Msg ? Msg : "(null)");
    LLVMDisposeErrorMessage(Msg);
    LLVMOrcDisposeLLJIT(JIT);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return nullptr;
  }

  P->JITs.push_back(JIT);
  LLVMOrcDisposeThreadSafeContext(TSCtx);

  void *Entry = reinterpret_cast<void *>(static_cast<uintptr_t>(Addr));
  spdlog::info("tier2-osr: compiled func {} loop {} → {:#x}", FuncIdx,
               LoopIdx, reinterpret_cast<uintptr_t>(Entry));
  return Entry;
}

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
