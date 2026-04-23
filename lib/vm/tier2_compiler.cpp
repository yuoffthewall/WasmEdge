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

/// Stub the body of every defined function in \p Mini whose module-wide
/// index is NOT in \p BatchSet. The replacement body pushes the function's
/// declared return values (default-constructed) and ends — type-checks
/// against the original signature without forcing `noreturn` semantics
/// (an `unreachable` stub would let LLVM infer noreturn and DCE batch
/// callsites that follow). Referenced non-batch entries are rewritten to
/// tier-2 → tier-1 t1_thunks after LLVM codegen.
void stubNonBatchInPlace(AST::Module &Mini,
                         const std::unordered_set<uint32_t> &BatchSet,
                         uint32_t ImportFuncNum) {
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
// The thunk is named `f<ThunkIdx>_fwd_thunk` (so the ORC symbol lookup
// finds it under the tier-1 FuncTable index) and invokes `f<CalleeIdx>`
// in the module. These are the same for regular batch compiles; the OSR
// path passes `CalleeIdx` = the synthesized OSR slot so the thunk still
// hangs off the original tier-1 index but calls the rewritten body.
void emitFwdThunk(LLVMModuleRef LLMod, LLVMContextRef Ctx, uint32_t ThunkIdx,
                  const AST::FunctionType &FT, uint32_t CalleeIdx) {
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
      "f" + std::to_string(ThunkIdx) + "_fwd_thunk";
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

  // Look up the target function `f<CalleeIdx>` already in the module.
  const std::string CalleeName = "f" + std::to_string(CalleeIdx);
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
        LLVMConstInt(LLVMInt32TypeInContext(Ctx), ThunkIdx, 0),
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
                    ThunkIdx);
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
                    ThunkIdx);
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
/// The OSR body is appended to the module as a *new* function slot
/// (OsrFuncIdxOut), leaving the original function at \p FuncIdx with its
/// original type and body. This matters because the rewritten OSR body
/// often contains self-recursive `call FuncIdx` instructions (quicksort,
/// fib2, etc.) — if we clobbered FuncIdx's type with the OSR signature
/// those calls would fail validation (stack underflow: OSR type has N
/// flattened-locals params but the call site only pushes the original
/// param count). Leaving FuncIdx alone lets the non-batch stubbing loop
/// + emitT1ThunkInPlace route self-recursion back to tier-1.
///
/// Caller is responsible for stubbing non-batch bodies (via
/// synthesizeMiniModule) and rewriting them as tier-2 → tier-1 bridges
/// (via emitT1ThunkInPlace) after LLVM codegen.
///
/// Returns true on success, false if the target loop was not found, the
/// enclosing scope chain contains an If (unsupported — would require
/// mid-entering a then/else arm), or validation prerequisites are not met.
bool appendOsrFunctionSlot(AST::Module &Mini, uint32_t FuncIdx,
                           uint32_t LoopIdx,
                           uint32_t ImportFuncNum,
                           uint32_t &OsrFuncIdxOut) noexcept {
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

  const auto &Seg = CodeSec[DefinedIdx];

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

  // Build the OSR body:
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

  // Append the OSR body as a new function slot. Keeps FuncIdx's original
  // body and type in place so in-body `call FuncIdx` instructions (self-
  // recursion) validate against the original signature.
  AST::CodeSegment OsrSeg;
  OsrSeg.getExpr().getInstrs() = std::move(OsrBody);
  CodeSec.push_back(std::move(OsrSeg));
  FuncSec.push_back(NewTypeIdx);
  OsrFuncIdxOut =
      ImportFuncNum + static_cast<uint32_t>(CodeSec.size()) - 1;

  // Re-point local refs that the dump-block below reads back so it
  // inspects the new OSR segment rather than the untouched original.
  auto &OsrSegRef = CodeSec.back();

  // Optional dump for debugging. WASMEDGE_OSR_DUMP=<dir> writes one file
  // per OSR body, named `osr_<funcIdx>_<loopIdx>.txt`, so subsequent
  // crashes don't eat the output before it's flushed.
  if (const char *Dir = std::getenv("WASMEDGE_OSR_DUMP")) {
    std::string Path = std::string(Dir) + "/osr_" +
                       std::to_string(FuncIdx) + "_" +
                       std::to_string(LoopIdx) + ".txt";
    if (FILE *F = std::fopen(Path.c_str(), "w")) {
      const auto &Body = OsrSegRef.getExpr().getInstrs();
      std::fprintf(F,
                   "func=%u osr_idx=%u loop=%u body_size=%zu rets=%zu "
                   "params=%zu\n",
                   FuncIdx, OsrFuncIdxOut, LoopIdx, Body.size(),
                   OrigRets.size(), OsrParams.size());
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

Expect<Tier2CompileResult>
Tier2Compiler::compileRequest(uint32_t RootFuncIdx,
                              Span<const uint32_t> Batch,
                              Span<const uint32_t> LoopEntries,
                              const AST::Module &Mod, unsigned OptLevel) {
  Tier2CompileResult Result;
  if (isShutdown() || Batch.empty()) {
    return Result;
  }

  // Derive import func count from the full module.
  uint32_t ImportFuncNum = 0;
  for (const auto &ImpDesc : Mod.getImportSection().getContent()) {
    if (ImpDesc.getExternalType() == ExternalType::Function) {
      ++ImportFuncNum;
    }
  }

  const bool HasOsr = !LoopEntries.empty();
  if (HasOsr && RootFuncIdx < ImportFuncNum) {
    spdlog::warn("tier2: OSR for func {} rejected (import index)",
                 RootFuncIdx);
    return Result;
  }

  // Mini starts as a full copy. Append OSR slots first so they read the
  // un-stubbed bodies of their root functions (the appended slot's body is
  // a prefix of the original body). Each appended slot's funcIdx is
  // assigned in the order pushed.
  AST::Module Mini(Mod);
  std::vector<std::pair<uint32_t, uint32_t>> OsrSlots; // (loopIdx, slotIdx)
  if (HasOsr) {
    OsrSlots.reserve(LoopEntries.size());
    for (uint32_t L : LoopEntries) {
      uint32_t Slot = UINT32_MAX;
      if (!appendOsrFunctionSlot(Mini, RootFuncIdx, L, ImportFuncNum, Slot)) {
        spdlog::warn("tier2-osr: synth failed for func {} loop {}",
                     RootFuncIdx, L);
        // Bail entirely — tier-2 batch will be re-requested on the next
        // call-count trip if the regular path wants this neighborhood.
        return Result;
      }
      OsrSlots.push_back({L, Slot});
    }
  }

  // Effective batch set:
  //   - Pure tier-2: every Batch member keeps real body and gets a fwd_thunk.
  //   - OSR: drop RootFuncIdx so self-recursion in the OSR body validates
  //     against the original signature and routes through a t1_thunk; add
  //     each appended OSR slot so its body stays in the mini-module.
  std::unordered_set<uint32_t> BatchSet;
  BatchSet.reserve(Batch.size() + OsrSlots.size());
  for (uint32_t F : Batch) {
    if (HasOsr && F == RootFuncIdx)
      continue;
    BatchSet.insert(F);
  }
  for (const auto &[_, Slot] : OsrSlots) {
    BatchSet.insert(Slot);
  }

  stubNonBatchInPlace(Mini, BatchSet, ImportFuncNum);

  // OSR rewrites a function body (locals → params, enclosing Block chain
  // synthesized) in a way that could fail validation if operand-stack
  // balance diverges from the original. Validate so we surface a clean
  // reject instead of an LLVM-backend assertion crash.
  if (HasOsr) {
    Configure ValConf;
    Validator::Validator V(ValConf);
    if (auto VR = V.validate(Mini); !VR) {
      spdlog::warn(
          "tier2-osr: validator rejected synthesized module (func {}): {}",
          RootFuncIdx, ErrCodeStr[VR.error().getEnum()]);
      return Result;
    }
  }
  Mini.setIsValidated(true);

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
    spdlog::warn("tier2: LLVM frontend compile failed (root func {})",
                 RootFuncIdx);
    return Result;
  }

  LLVMModuleRef LLMod = CompRes->getModuleRef();
  LLVMContextRef LLCtx = CompRes->getContextRef();
  if (!LLMod || !LLCtx) {
    spdlog::error("tier2: compiled Data has no LLVM module/context");
    return Result;
  }

  // Emit fwd_thunks. Two flavors:
  //   - Pure tier-2: one per Batch member, ThunkIdx == CalleeIdx.
  //   - OSR: one per appended slot, ThunkIdx = RootFuncIdx (so the tier-1
  //     runtime finds it via the same lookup name), CalleeIdx = OsrSlot.
  //     Single-loop requests are the only supported shape today; multi-
  //     loop in one Request would collide on `f<RootFuncIdx>_fwd_thunk`.
  if (HasOsr) {
    for (const auto &[L, Slot] : OsrSlots) {
      const auto &OsrFT = getFuncType(Mini, Slot, ImportFuncNum);
      emitFwdThunk(LLMod, LLCtx, RootFuncIdx, OsrFT, Slot);
      (void)L;
    }
  } else {
    for (uint32_t FuncIdx : Batch) {
      const auto &FT = getFuncType(Mod, FuncIdx, ImportFuncNum);
      emitFwdThunk(LLMod, LLCtx, FuncIdx, FT, FuncIdx);
    }
  }

  // Replace the stub body of every non-batch defined function that is
  // referenced by a batch body with a tier-2 → tier-1 dispatch thunk.
  // This is what makes cross-batch calls safe: without this pass the
  // batch would execute a `[const, end]` stub and silently drop the
  // call, corrupting the caller's logical state.
  const auto NonBatchCallees =
      collectNonBatchCallees(Mini, BatchSet, ImportFuncNum);
  for (uint32_t CalleeIdx : NonBatchCallees) {
    // Use the ORIGINAL module's FuncType — Mini's stubbed bodies keep the
    // original signature, and t1_thunks dispatch by funcIdx at runtime
    // against the tier-1 FuncTable which uses the original types.
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
  for (uint32_t BIdx : BatchSet) {
    std::string Name = "f" + std::to_string(BIdx);
    LLVMValueRef Fn = LLVMGetNamedFunction(LLMod, Name.c_str());
    if (!Fn)
      continue;
    LLVMSetLinkage(Fn, LLVMInternalLinkage);
    LLVMSetVisibility(Fn, LLVMDefaultVisibility);
    LLVMSetDLLStorageClass(Fn, LLVMDefaultStorageClass);
  }

  // Prune unused stub functions before running the O2 pipeline. The
  // mini-module keeps a slot at every original funcIdx so the frontend's
  // call lowering resolves, but on wide call graphs (e.g. rust-*, 350-
  // 900 functions) almost every slot is a `ret iN 0` stub that nothing
  // calls — the batch body only reaches O(10) direct callees, which
  // emitT1ThunkInPlace above rewrites in-place (leaving those functions
  // with live uses). Everything else is dead weight that LLVM otherwise
  // drags through its interprocedural passes.
  //
  // Match exactly the stub naming pattern (`f` followed by digits, not
  // ending in `_fwd_thunk`) and erase only if `use_empty()`. Batch
  // members keep the fwd_thunk's single use; t1_thunks keep their batch
  // callers' uses; fwd_thunks are excluded by name (external callers via
  // ORC produce no in-module use). Trap stubs (`tN`) and runtime helpers
  // (`wasmedge_*`) are left alone.
  auto IsStubFuncName = [](const char *Name, size_t Len) {
    if (Len < 2 || Name[0] != 'f')
      return false;
    for (size_t I = 1; I < Len; ++I) {
      if (Name[I] < '0' || Name[I] > '9')
        return false;
    }
    return true;
  };
  unsigned Pruned = 0;
  {
    LLVMValueRef Fn = LLVMGetFirstFunction(LLMod);
    while (Fn) {
      LLVMValueRef Next = LLVMGetNextFunction(Fn);
      if (!LLVMIsDeclaration(Fn)) {
        size_t NameLen = 0;
        const char *Name = LLVMGetValueName2(Fn, &NameLen);
        if (IsStubFuncName(Name, NameLen) &&
            LLVMGetFirstUse(Fn) == nullptr) {
          LLVMDeleteFunction(Fn);
          ++Pruned;
        }
      }
      Fn = Next;
    }
  }
  if (Pruned > 0) {
    spdlog::info("tier2: pruned {} dead stub functions before opt",
                 Pruned);
  }

  // Run LLVM opt on the post-processed module now that all thunks and
  // stub rewrites are in place. The WasmEdge frontend was invoked at O0,
  // so call sites to non-batch stubs (now t1_thunks) are still intact.
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
    if (HasOsr) {
      Path += "tier2_osr_f" + std::to_string(RootFuncIdx) + "_l" +
              std::to_string(OsrSlots.front().first) + ".ll";
    } else {
      Path += "tier2_f" + std::to_string(RootFuncIdx) + ".ll";
    }
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

  // Install intrinsics table.
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

  // Look up emitted thunk addresses.
  if (HasOsr) {
    const std::string ThunkName =
        "f" + std::to_string(RootFuncIdx) + "_fwd_thunk";
    LLVMOrcJITTargetAddress Addr = 0;
    if (LLVMErrorRef Err =
            LLVMOrcLLJITLookup(JIT, &Addr, ThunkName.c_str())) {
      char *Msg = LLVMGetErrorMessage(Err);
      spdlog::warn("tier2-osr: lookup {} failed: {}", ThunkName,
                   Msg ? Msg : "(null)");
      LLVMDisposeErrorMessage(Msg);
      LLVMOrcDisposeLLJIT(JIT);
      LLVMOrcDisposeThreadSafeContext(TSCtx);
      return Result;
    }
    void *Entry = reinterpret_cast<void *>(static_cast<uintptr_t>(Addr));
    Result.OsrEntries.reserve(OsrSlots.size());
    for (const auto &[L, _] : OsrSlots) {
      Result.OsrEntries.emplace_back(L, Entry);
    }
  } else {
    Result.FwdThunks.reserve(Batch.size());
    for (uint32_t FuncIdx : Batch) {
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
      Result.FwdThunks.emplace_back(
          FuncIdx, reinterpret_cast<void *>(static_cast<uintptr_t>(Addr)));
    }
  }

  // Retain the JIT for the lifetime of this Tier2Compiler — the code
  // returned to the FuncTable swap lives inside it.
  P->JITs.push_back(JIT);
  LLVMOrcDisposeThreadSafeContext(TSCtx);

  if (HasOsr) {
    spdlog::info("tier2-osr: ORC done for func {} (entries {})", RootFuncIdx,
                 Result.OsrEntries.size());
  } else {
    spdlog::info("tier2: ORC done for func {} (emitted {} thunks)",
                 RootFuncIdx, Result.FwdThunks.size());
  }
  return Result;
}

namespace {

/// Emit one LLVM-ABI entry thunk that forwards to a tier-1 IR-JIT native
/// pointer. Signature: `ret (ExecCtx*, typed_params...)`. Body:
///   1. Inline `movq %fs:OFFSET, $0` to load `wasmedge_tier2_jit_env_tls`
///      (the JitExecEnv* installed by IRJitEngine::invoke).
///   2. Allocate `uint64_t args[NParams]` on the stack, store each
///      typed param at offset `I*8`.
///   3. Indirect `fastcall` to the embedded tier-1 native pointer with
///      `(JitExecEnv*, uint64_t*)` signature.
///   4. Narrow the widened tier-1 return into the LLVM-native return
///      type and emit `ret`.
/// Returns `false` if the function signature contains non-scalar types
/// (ref/v128/multi-return); caller must skip those FuncIdxs.
bool emitIRJitEntryThunk(LLVMModuleRef LLMod, LLVMContextRef Ctx,
                          uint32_t FuncIdx, const AST::FunctionType &FT,
                          void *IRJitNative) {
  const auto &Params = FT.getParamTypes();
  const auto &Rets = FT.getReturnTypes();

  auto IsScalar = [](TypeCode C) {
    return C == TypeCode::I32 || C == TypeCode::I64 ||
           C == TypeCode::F32 || C == TypeCode::F64;
  };
  if (Rets.size() > 1) return false;
  for (const auto &P : Params)
    if (!IsScalar(P.getCode())) return false;
  if (!Rets.empty() && !IsScalar(Rets.front().getCode())) return false;

  LLVMTypeRef PtrTy = LLVMPointerTypeInContext(Ctx, 0);
  LLVMTypeRef Int64Ty = LLVMInt64TypeInContext(Ctx);
  LLVMTypeRef Int32Ty = LLVMInt32TypeInContext(Ctx);
  LLVMTypeRef FloatTy = LLVMFloatTypeInContext(Ctx);
  LLVMTypeRef DoubleTy = LLVMDoubleTypeInContext(Ctx);
  LLVMTypeRef VoidTy = LLVMVoidTypeInContext(Ctx);

  // Build the LLVM-native signature expected by compileIndirectCallOp's
  // NotNullBB: (ExecCtx*, param0, param1, …) → ret.
  std::vector<LLVMTypeRef> ParamTys;
  ParamTys.reserve(Params.size() + 1);
  ParamTys.push_back(PtrTy); // ExecCtx*
  for (const auto &P : Params) {
    switch (P.getCode()) {
    case TypeCode::I32: ParamTys.push_back(Int32Ty); break;
    case TypeCode::I64: ParamTys.push_back(Int64Ty); break;
    case TypeCode::F32: ParamTys.push_back(FloatTy); break;
    case TypeCode::F64: ParamTys.push_back(DoubleTy); break;
    default: return false;
    }
  }
  LLVMTypeRef LlvmRetTy = VoidTy;
  if (!Rets.empty()) {
    switch (Rets.front().getCode()) {
    case TypeCode::I32: LlvmRetTy = Int32Ty; break;
    case TypeCode::I64: LlvmRetTy = Int64Ty; break;
    case TypeCode::F32: LlvmRetTy = FloatTy; break;
    case TypeCode::F64: LlvmRetTy = DoubleTy; break;
    default: return false;
    }
  }

  LLVMTypeRef FTy = LLVMFunctionType(
      LlvmRetTy, ParamTys.data(),
      static_cast<unsigned>(ParamTys.size()), /*IsVarArg=*/0);

  const std::string Name = "f" + std::to_string(FuncIdx) + "_entry_thunk";
  LLVMValueRef Thunk = LLVMAddFunction(LLMod, Name.c_str(), FTy);
  LLVMSetLinkage(Thunk, LLVMExternalLinkage);
  {
    unsigned UWTableKind = LLVMGetEnumAttributeKindForName(
        "uwtable", static_cast<unsigned>(std::strlen("uwtable")));
    LLVMAddAttributeAtIndex(
        Thunk, LLVMAttributeFunctionIndex,
        LLVMCreateEnumAttribute(Ctx, UWTableKind, 0));
  }

  LLVMBasicBlockRef BB = LLVMAppendBasicBlockInContext(Ctx, Thunk, "entry");
  LLVMBuilderRef B = LLVMCreateBuilderInContext(Ctx);
  LLVMPositionBuilderAtEnd(B, BB);

  // Inline asm: `movq %fs:OFFSET, $0` → JitExecEnv* (tier-1 TLS).
  static const ptrdiff_t TlsOff =
      wasmedge_tier2_get_jit_env_tls_offset();
  LLVMTypeRef AsmFTy = LLVMFunctionType(PtrTy, nullptr, 0, /*IsVarArg=*/0);
  std::string AsmStr = "movq %fs:" + std::to_string(TlsOff) + ", $0";
  std::string AsmCon = "=r";
  LLVMValueRef AsmVal = LLVMGetInlineAsm(
      AsmFTy, AsmStr.c_str(), AsmStr.size(),
      AsmCon.c_str(), AsmCon.size(),
      /*HasSideEffects=*/0, /*IsAlignStack=*/0,
      LLVMInlineAsmDialectATT, /*CanThrow=*/0);
  LLVMValueRef Env =
      LLVMBuildCall2(B, AsmFTy, AsmVal, nullptr, 0, "jit_env");

  // Allocate 8-byte-slot args[] scratch.
  LLVMValueRef ArgsArr = nullptr;
  if (!Params.empty()) {
    ArgsArr = LLVMBuildArrayAlloca(
        B, Int64Ty,
        LLVMConstInt(Int32Ty, static_cast<uint32_t>(Params.size()), 0),
        "args");
  } else {
    ArgsArr = LLVMConstNull(PtrTy);
  }

  // Marshal each typed param into args[i] as u64 (low-bits convention;
  // tier-1 `ir_LOAD_I32`/`ir_LOAD_F` reads low 4 bytes, high bits are
  // don't-care — same contract emitFwdThunk establishes in reverse).
  for (size_t I = 0; I < Params.size(); ++I) {
    LLVMValueRef Raw = nullptr;
    LLVMValueRef P = LLVMGetParam(Thunk, static_cast<unsigned>(I + 1));
    switch (Params[I].getCode()) {
    case TypeCode::I32:
      Raw = LLVMBuildZExt(B, P, Int64Ty, "p64");
      break;
    case TypeCode::I64:
      Raw = P;
      break;
    case TypeCode::F32: {
      LLVMValueRef Bits = LLVMBuildBitCast(B, P, Int32Ty, "fbits");
      Raw = LLVMBuildZExt(B, Bits, Int64Ty, "p64");
      break;
    }
    case TypeCode::F64:
      Raw = LLVMBuildBitCast(B, P, Int64Ty, "p64");
      break;
    default:
      LLVMDisposeBuilder(B);
      return false;
    }
    LLVMValueRef Idx = LLVMConstInt(Int64Ty, I, 0);
    LLVMValueRef Slot =
        LLVMBuildInBoundsGEP2(B, Int64Ty, ArgsArr, &Idx, 1, "slot");
    LLVMBuildStore(B, Raw, Slot);
  }

  // Tier-1 signature: ret_widened fastcall(JitExecEnv*, uint64_t*).
  LLVMTypeRef Tier1RetTy = VoidTy;
  if (!Rets.empty()) {
    switch (Rets.front().getCode()) {
    case TypeCode::I32:
    case TypeCode::I64:
      Tier1RetTy = Int64Ty;
      break;
    case TypeCode::F32:
      Tier1RetTy = FloatTy;
      break;
    case TypeCode::F64:
      Tier1RetTy = DoubleTy;
      break;
    default: break;
    }
  }
  LLVMTypeRef Tier1ParamTys[2] = {PtrTy, PtrTy};
  LLVMTypeRef Tier1FTy =
      LLVMFunctionType(Tier1RetTy, Tier1ParamTys, 2, /*IsVarArg=*/0);

  // Embed the tier-1 native pointer as a constant.
  LLVMValueRef Target = LLVMConstIntToPtr(
      LLVMConstInt(Int64Ty,
                    static_cast<uint64_t>(
                        reinterpret_cast<uintptr_t>(IRJitNative)),
                    0),
      PtrTy);

  LLVMValueRef CallArgs[2] = {Env, ArgsArr};
  LLVMValueRef CallRes = LLVMBuildCall2(
      B, Tier1FTy, Target, CallArgs, 2, Rets.empty() ? "" : "tr");

  // Narrow tier-1's widened return into the LLVM-native return type.
  if (Rets.empty()) {
    LLVMBuildRetVoid(B);
  } else {
    LLVMValueRef Out = nullptr;
    switch (Rets.front().getCode()) {
    case TypeCode::I32:
      Out = LLVMBuildTrunc(B, CallRes, Int32Ty, "i32r");
      break;
    case TypeCode::I64:
    case TypeCode::F32:
    case TypeCode::F64:
      Out = CallRes;
      break;
    default:
      LLVMDisposeBuilder(B);
      return false;
    }
    LLVMBuildRet(B, Out);
  }

  LLVMDisposeBuilder(B);
  return true;
}

} // namespace

Expect<IRJitEntryThunksResult>
buildIRJitEntryThunks(Span<const IRJitEntryThunkReq> Reqs) {
  IRJitEntryThunksResult Out;
  if (Reqs.empty()) {
    return Out;
  }

  // Target init. idempotent; safe to call repeatedly.
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();

  LLVMOrcThreadSafeContextRef TSCtx = LLVMOrcCreateNewThreadSafeContext();
  LLVMContextRef Ctx = LLVMOrcThreadSafeContextGetContext(TSCtx);
  LLVMModuleRef LLMod =
      LLVMModuleCreateWithNameInContext("ir_jit_entry_thunks", Ctx);

  std::vector<uint32_t> EmittedIdxs;
  EmittedIdxs.reserve(Reqs.size());
  for (const auto &R : Reqs) {
    if (!R.NativeFunc || !R.FuncType) continue;
    if (emitIRJitEntryThunk(LLMod, Ctx, R.FuncIdx, *R.FuncType, R.NativeFunc)) {
      EmittedIdxs.push_back(R.FuncIdx);
    }
  }

  if (EmittedIdxs.empty()) {
    LLVMDisposeModule(LLMod);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Out;
  }

  // Wrap module into a ThreadSafeModule — addLLVMIRModule takes ownership.
  LLVMOrcThreadSafeModuleRef TSM =
      LLVMOrcCreateNewThreadSafeModule(LLMod, TSCtx);

  LLVMOrcLLJITBuilderRef Builder = LLVMOrcCreateLLJITBuilder();
  LLVMOrcLLJITRef JIT = nullptr;
  if (LLVMErrorRef Err = LLVMOrcCreateLLJIT(&JIT, Builder)) {
    char *Msg = LLVMGetErrorMessage(Err);
    spdlog::error("ir-jit entry thunks: LLJIT create failed: {}",
                  Msg ? Msg : "(null)");
    LLVMDisposeErrorMessage(Msg);
    LLVMOrcDisposeThreadSafeModule(TSM);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  LLVMOrcJITDylibRef MainJD = LLVMOrcLLJITGetMainJITDylib(JIT);
  if (LLVMErrorRef Err = LLVMOrcLLJITAddLLVMIRModule(JIT, MainJD, TSM)) {
    char *Msg = LLVMGetErrorMessage(Err);
    spdlog::error("ir-jit entry thunks: addLLVMIRModule failed: {}",
                  Msg ? Msg : "(null)");
    LLVMDisposeErrorMessage(Msg);
    LLVMOrcDisposeLLJIT(JIT);
    LLVMOrcDisposeThreadSafeContext(TSCtx);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  for (uint32_t FuncIdx : EmittedIdxs) {
    const std::string Name =
        "f" + std::to_string(FuncIdx) + "_entry_thunk";
    LLVMOrcJITTargetAddress Addr = 0;
    if (LLVMErrorRef Err = LLVMOrcLLJITLookup(JIT, &Addr, Name.c_str())) {
      char *Msg = LLVMGetErrorMessage(Err);
      spdlog::warn("ir-jit entry thunks: lookup {} failed: {}", Name,
                   Msg ? Msg : "(null)");
      LLVMDisposeErrorMessage(Msg);
      continue;
    }
    Out.Thunks.emplace_back(FuncIdx,
                            reinterpret_cast<void *>(
                                static_cast<uintptr_t>(Addr)));
  }

  // Hand ownership of the LLJIT to a shared_ptr with a custom deleter.
  // The TSCtx lifetime is tied to the JIT (we dispose it when the JIT is
  // destroyed).
  struct KeepAlive {
    LLVMOrcLLJITRef JIT;
    LLVMOrcThreadSafeContextRef TSCtx;
  };
  auto *K = new KeepAlive{JIT, TSCtx};
  Out.Keepalive = std::shared_ptr<void>(
      static_cast<void *>(K), [](void *P) noexcept {
        auto *KA = static_cast<KeepAlive *>(P);
        if (KA->JIT) LLVMOrcDisposeLLJIT(KA->JIT);
        if (KA->TSCtx) LLVMOrcDisposeThreadSafeContext(KA->TSCtx);
        delete KA;
      });

  spdlog::info("ir-jit entry thunks: emitted {}/{} thunks",
               Out.Thunks.size(), Reqs.size());
  return Out;
}

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT && WASMEDGE_USE_LLVM
