// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/tier2_compiler.h"

#if defined(WASMEDGE_BUILD_IR_JIT) && defined(WASMEDGE_USE_LLVM)

#include "ast/module.h"
#include "common/configure.h"
#include "executor/executor.h"
#include "llvm/compiler.h"
#include "llvm/data.h"

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
#include <string>
#include <unordered_set>
#include <vector>

// Tier-2 TLS accessors. Both are bound as ORC absolute symbols below; the
// JIT'd code calls them to obtain its thread-local state.
//   wasmedge_tier2_get_jit_env  → g_tier2_current_env (JitExecEnv*)
//   wasmedge_tier2_get_exec_ctx → &Executor::ExecutionContext
// We deliberately go through a function call rather than emitting direct
// %fs:OFFSET inline asm. Inline %fs:OFFSET assumes TLS lives in the static
// TLS block at a fixed offset from the thread pointer — that holds for
// statically-linked binaries but breaks when WasmEdge is loaded as a
// dlopen'd shared library (where TLS variables live in dynamically-
// allocated DTV chunks resolved through __tls_get_addr / TLSDESC). The
// function-call overhead is on a cross-batch dispatch boundary, not in
// inner loops, so the cost is negligible.
extern "C" void *wasmedge_tier2_get_jit_env(void);
extern "C" void *wasmedge_tier2_get_exec_ctx(void);

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

  // Declare the helper that returns &Executor::ExecutionContext. We bind
  // this to its real address via ORC absolute symbols at JIT time.
  LLVMValueRef GetCtx = LLVMGetNamedFunction(LLMod, "wasmedge_tier2_get_exec_ctx");
  if (!GetCtx) {
    LLVMTypeRef GetCtxFTy = LLVMFunctionType(PtrTy, nullptr, 0, 0);
    GetCtx = LLVMAddFunction(LLMod, "wasmedge_tier2_get_exec_ctx", GetCtxFTy);
    LLVMSetLinkage(GetCtx, LLVMExternalLinkage);
  }
  LLVMTypeRef GetCtxFTy = LLVMFunctionType(PtrTy, nullptr, 0, 0);

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

  // %exec_ctx = call ptr @wasmedge_tier2_get_exec_ctx()
  LLVMValueRef ExecCtx =
      LLVMBuildCall2(B, GetCtxFTy, GetCtx, nullptr, 0, "exec_ctx");

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

  // Declare the env helper on demand.
  LLVMValueRef GetEnv =
      LLVMGetNamedFunction(LLMod, "wasmedge_tier2_get_jit_env");
  LLVMTypeRef GetEnvFTy = LLVMFunctionType(PtrTy, nullptr, 0, 0);
  if (!GetEnv) {
    GetEnv =
        LLVMAddFunction(LLMod, "wasmedge_tier2_get_jit_env", GetEnvFTy);
    LLVMSetLinkage(GetEnv, LLVMExternalLinkage);
  }

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

  // %env = call ptr @wasmedge_tier2_get_jit_env()
  LLVMValueRef Env = LLVMBuildCall2(B, GetEnvFTy, GetEnv, nullptr, 0, "env");

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

/// Walk the bodies of every function in \p BatchSet and collect all direct
/// `call` targets that are (a) defined (not imports) and (b) not already
/// in the batch. These are the non-batch functions for which we must
/// emit tier-2 → tier-1 bridges.
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

  // Run LLVM opt on the post-processed module now that all thunks and
  // stub rewrites are in place. The WasmEdge frontend was invoked at O0,
  // so call sites to non-batch stubs (now t1_thunks) are still intact.
  // Opt here is free to inline t1_thunks where profitable.
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

  // Define the two tier-2 helpers as absolute symbols:
  //   - wasmedge_tier2_get_exec_ctx: returns &Executor::ExecutionContext,
  //     used by fwd_thunks to build a valid ExecCtxPtrTy when tier-1
  //     dispatches into tier-2 code via the FuncTable.
  //   - wasmedge_tier2_get_jit_env:  returns the thread-local JitExecEnv*
  //     set by IRJitEngine::invoke, used by t1_thunks to dispatch tier-2
  //     → tier-1 calls via FuncTable[idx] with the tier-1 ABI.
  {
    // ORC absolute-symbol bindings for tier-2 helpers the JIT'd code calls
    // by name. Bound here (rather than relying on dlsym(RTLD_DEFAULT, ...))
    // so resolution works regardless of how the WasmEdge libraries are
    // linked into the host process — including dlopen'd shared libraries
    // built with `-fvisibility=hidden`. wasmedge_tier2_get_exec_ctx is
    // defined in proxy.cpp; the other two live in ir_jit_engine.cpp.
    struct AbsSym {
      const char *Name;
      void *Addr;
    };
    const AbsSym Syms[] = {
        {"wasmedge_tier2_trace_thunk",
         reinterpret_cast<void *>(&wasmedge_tier2_trace_thunk)},
        {"wasmedge_tier2_get_jit_env",
         reinterpret_cast<void *>(&wasmedge_tier2_get_jit_env)},
        {"wasmedge_tier2_get_exec_ctx",
         reinterpret_cast<void *>(&wasmedge_tier2_get_exec_ctx)},
    };
    constexpr size_t NumSyms = sizeof(Syms) / sizeof(Syms[0]);
    LLVMOrcCSymbolMapPair Pairs[NumSyms];
    for (size_t I = 0; I < NumSyms; ++I) {
      Pairs[I].Name = LLVMOrcExecutionSessionIntern(ES, Syms[I].Name);
      Pairs[I].Sym.Address =
          reinterpret_cast<LLVMOrcJITTargetAddress>(Syms[I].Addr);
      Pairs[I].Sym.Flags.GenericFlags = LLVMJITSymbolGenericFlagsExported |
                                         LLVMJITSymbolGenericFlagsCallable;
      Pairs[I].Sym.Flags.TargetFlags = 0;
    }

    LLVMOrcMaterializationUnitRef MU = LLVMOrcAbsoluteSymbols(Pairs, NumSyms);
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

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
