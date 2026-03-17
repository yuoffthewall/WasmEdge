// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "executor/executor.h"

#include "common/spdlog.h"
#include "system/fault.h"
#include "system/stacktrace.h"

#ifdef WASMEDGE_BUILD_IR_JIT
#include "vm/ir_jit_engine.h"
#include <csetjmp>
#include <cstring>
#endif

#include <cstdint>
#include <utility>
#include <vector>

#ifdef WASMEDGE_BUILD_IR_JIT
static thread_local WasmEdge::Executor::Executor *g_jitExecutor = nullptr;
static thread_local WasmEdge::Runtime::StackManager *g_jitStackMgr = nullptr;
static thread_local const WasmEdge::Runtime::Instance::ModuleInstance *g_jitModInst = nullptr;
static thread_local WasmEdge::Runtime::Instance::MemoryInstance *g_jitMemory0 = nullptr;

extern "C" uint64_t jit_host_call(WasmEdge::VM::JitExecEnv *env,
                                  uint32_t funcIdx, uint64_t *args) {
  (void)env;
  if (!g_jitExecutor || !g_jitStackMgr || !g_jitModInst)
    return 0;

  const WasmEdge::Runtime::Instance::FunctionInstance *funcInst = nullptr;

  if (funcIdx & 0x80000000u) {
    // call_indirect: bit 31 set, lower bits = table slot index
    uint32_t tableSlot = funcIdx & 0x7FFFFFFFu;
    auto *tab = g_jitExecutor->getTabInstByIdx(*g_jitStackMgr, 0);
    if (!tab)
      return 0;
    auto refRes = tab->getRefAddr(tableSlot);
    if (!refRes)
      return 0;
    funcInst =
        (*refRes).getPtr<const WasmEdge::Runtime::Instance::FunctionInstance>();
    if (!funcInst)
      return 0;

    // Fast path: if the target is a JIT function, call it directly
    // instead of going through the executor (avoids deep stack recursion).
    if (funcInst->isIRJitFunction()) {
      void *nativeFunc = funcInst->getIRJitNativeFunc();
      if (nativeFunc) {
        const auto &ft = funcInst->getFuncType();
        const auto &retTypes = ft.getReturnTypes();
        if (retTypes.empty()) {
          using Fn = void (*)(WasmEdge::VM::JitExecEnv *, uint64_t *);
          reinterpret_cast<Fn>(nativeFunc)(env, args);
          return 0;
        } else {
          using Fn = uint64_t (*)(WasmEdge::VM::JitExecEnv *, uint64_t *);
          return reinterpret_cast<Fn>(nativeFunc)(env, args);
        }
      }
    }
  } else {
    auto funcs = g_jitModInst->getFunctionInstances();
    if (funcIdx >= funcs.size())
      return 0;
    funcInst = funcs[funcIdx];
    if (!funcInst)
      return 0;
  }

  const auto &funcType = funcInst->getFuncType();
  const auto &paramTypes = funcType.getParamTypes();
  const auto &retTypes = funcType.getReturnTypes();

  std::vector<WasmEdge::ValVariant> params(paramTypes.size());
  for (size_t i = 0; i < paramTypes.size(); ++i) {
    auto code = paramTypes[i].getCode();
    if (code == WasmEdge::TypeCode::F32) {
      float f;
      std::memcpy(&f, &args[i], sizeof(float));
      params[i] = WasmEdge::ValVariant(f);
    } else if (code == WasmEdge::TypeCode::F64) {
      double d;
      std::memcpy(&d, &args[i], sizeof(double));
      params[i] = WasmEdge::ValVariant(d);
    } else if (code == WasmEdge::TypeCode::I32) {
      params[i] = WasmEdge::ValVariant(static_cast<uint32_t>(args[i]));
    } else {
      params[i] = WasmEdge::ValVariant(static_cast<uint64_t>(args[i]));
    }
  }

  auto res = g_jitExecutor->jitCallFunction(*g_jitStackMgr, *funcInst, params,
                                            g_jitModInst);
  if (!res) {
    if (res.error() != WasmEdge::ErrCode::Value::Terminated) {
      spdlog::error("jit_host_call: func {} failed: {}", funcIdx,
                    WasmEdge::ErrCode(res.error()));
    }
    if (res.error() == WasmEdge::ErrCode::Value::Terminated) {
      void *buf = WasmEdge::VM::wasmedge_ir_jit_get_termination_buf();
      if (buf)
        longjmp(*static_cast<jmp_buf *>(buf), 1);
    }
    return 0;
  }

  uint64_t retVal = 0;
  if (!retTypes.empty()) {
    auto val = g_jitStackMgr->pop();
    auto code = retTypes[0].getCode();
    if (code == WasmEdge::TypeCode::F32) {
      float f = val.get<float>();
      std::memcpy(&retVal, &f, sizeof(float));
    } else if (code == WasmEdge::TypeCode::F64) {
      double d = val.get<double>();
      std::memcpy(&retVal, &d, sizeof(double));
    } else {
      retVal = val.get<uint64_t>();
    }
  }
  return retVal;
}

/// Trampoline for direct "call": if funcPtr is null (import), dispatch via
/// jit_host_call; otherwise call the JIT function. Prevents null deref when
/// ImportFuncNum is wrong or table entry is null.
/// retTypeCode: 0=void, 1=i32, 2=i64, 3=f32, 4=f64
extern "C" uint64_t jit_direct_or_host(WasmEdge::VM::JitExecEnv *env,
                                       void *funcPtr, uint32_t funcIdx,
                                       uint64_t *args, uint32_t retTypeCode) {
  if (!funcPtr)
    return jit_host_call(env, funcIdx, args);
  switch (retTypeCode) {
  case 0: {
    using Fn = void (*)(WasmEdge::VM::JitExecEnv *, uint64_t *);
    reinterpret_cast<Fn>(funcPtr)(env, args);
    return 0;
  }
  case 1: {
    using Fn = uint64_t (*)(WasmEdge::VM::JitExecEnv *, uint64_t *);
    return reinterpret_cast<Fn>(funcPtr)(env, args) & 0xFFFFFFFFu;
  }
  case 2: {
    using Fn = uint64_t (*)(WasmEdge::VM::JitExecEnv *, uint64_t *);
    return reinterpret_cast<Fn>(funcPtr)(env, args);
  }
  case 3: {
    using Fn = float (*)(WasmEdge::VM::JitExecEnv *, uint64_t *);
    float f = reinterpret_cast<Fn>(funcPtr)(env, args);
    uint64_t u;
    std::memcpy(&u, &f, sizeof(f));
    return u;
  }
  case 4: {
    using Fn = double (*)(WasmEdge::VM::JitExecEnv *, uint64_t *);
    double d = reinterpret_cast<Fn>(funcPtr)(env, args);
    uint64_t u;
    std::memcpy(&u, &d, sizeof(d));
    return u;
  }
  default:
    return 0;
  }
}

/// call_indirect trampoline: delegates to Executor::jitCallIndirect which has
/// access to protected ModuleInstance members for proper type checking.
extern "C" uint64_t jit_call_indirect(WasmEdge::VM::JitExecEnv *env,
                                       uint32_t tableIdx, uint32_t elemIdx,
                                       uint32_t typeIdx, uint64_t *args,
                                       uint32_t retTypeCode) {
  if (!g_jitExecutor || !g_jitStackMgr)
    return 0;
  auto res = g_jitExecutor->jitCallIndirect(*g_jitStackMgr, tableIdx, elemIdx,
                                            typeIdx, args, retTypeCode, env);
  if (!res) {
    if (res.error() == WasmEdge::ErrCode::Value::Terminated) {
      void *buf = WasmEdge::VM::wasmedge_ir_jit_get_termination_buf();
      if (buf)
        longjmp(*static_cast<jmp_buf *>(buf), 1);
    }
    // For traps (UndefinedElement, UninitializedElement, IndirectCallTypeMismatch),
    // longjmp to terminate the JIT frame.
    void *buf = WasmEdge::VM::wasmedge_ir_jit_get_termination_buf();
    if (buf)
      longjmp(*static_cast<jmp_buf *>(buf), 1);
    return 0;
  }
  return *res;
}

extern "C" int32_t jit_memory_grow(WasmEdge::VM::JitExecEnv *env,
                                    uint32_t nPages) {
  if (!g_jitMemory0)
    return static_cast<int32_t>(-1);
  uint32_t oldPages = g_jitMemory0->getPageSize();
  if (!g_jitMemory0->growPage(nPages))
    return static_cast<int32_t>(-1);
  // Update MemoryBase in env since grow may relocate the buffer.
  if (env)
    env->MemoryBase = g_jitMemory0->getDataPtr();
  return static_cast<int32_t>(oldPages);
}

extern "C" int32_t jit_memory_size(WasmEdge::VM::JitExecEnv *env) {
  (void)env;
  if (!g_jitMemory0)
    return 0;
  return static_cast<int32_t>(g_jitMemory0->getPageSize());
}

extern "C" void jit_memory_copy(WasmEdge::VM::JitExecEnv *env,
                                uint32_t dstMemIdx, uint32_t srcMemIdx,
                                uint32_t dst, uint32_t src, uint32_t len) {
  (void)env;
  if (!g_jitExecutor || !g_jitStackMgr)
    return;
  auto *memDst = g_jitExecutor->getMemInstByIdx(*g_jitStackMgr, dstMemIdx);
  auto *memSrc = g_jitExecutor->getMemInstByIdx(*g_jitStackMgr, srcMemIdx);
  if (!memDst || !memSrc) {
    spdlog::error(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
    return;
  }
  auto dataRes = memSrc->getBytes(src, len);
  if (!dataRes) {
    spdlog::error(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
    return;
  }
  auto setRes = memDst->setBytes(*dataRes, dst, 0, len);
  if (!setRes) {
    spdlog::error(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
  }
}

extern "C" void jit_memory_fill(WasmEdge::VM::JitExecEnv *env, uint32_t memIdx,
                                uint32_t off, uint32_t val, uint32_t len) {
  (void)env;
  if (!g_jitExecutor || !g_jitStackMgr)
    return;
  auto *mem = g_jitExecutor->getMemInstByIdx(*g_jitStackMgr, memIdx);
  if (!mem) {
    spdlog::error(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
    return;
  }
  auto res = mem->fillBytes(static_cast<uint8_t>(val), off, len);
  if (!res) {
    spdlog::error(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
  }
}

extern "C" void jit_ref_func(WasmEdge::VM::JitExecEnv *env, uint32_t funcIdx) {
  if (!env || !g_jitModInst)
    return;
  auto funcs = g_jitModInst->getFunctionInstances();
  if (funcIdx >= funcs.size())
    return;
  const auto *funcInst = funcs[funcIdx];
  WasmEdge::RefVariant ref(funcInst);
  uint64_t *out = env->RefResultBuf;
  auto raw = ref.getRawData();
  out[0] = raw[0];
  out[1] = raw[1];
}

extern "C" void jit_table_get(WasmEdge::VM::JitExecEnv *env, uint32_t tableIdx,
                              uint32_t idx) {
  if (!env || !g_jitExecutor || !g_jitStackMgr)
    return;
  auto *tab = g_jitExecutor->getTabInstByIdx(*g_jitStackMgr, tableIdx);
  if (!tab) {
    spdlog::error(WasmEdge::ErrCode::Value::TableOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::TableOutOfBounds);
  }
  auto res = tab->getRefAddr(idx);
  if (!res) {
    spdlog::error(WasmEdge::ErrCode::Value::TableOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::TableOutOfBounds);
  }
  auto raw = (*res).getRawData();
  env->RefResultBuf[0] = raw[0];
  env->RefResultBuf[1] = raw[1];
}

extern "C" void jit_table_set(WasmEdge::VM::JitExecEnv *env, uint32_t tableIdx,
                              uint32_t idx, const uint64_t *refPtr) {
  (void)env;
  if (!refPtr || !g_jitExecutor || !g_jitStackMgr)
    return;
  auto *tab = g_jitExecutor->getTabInstByIdx(*g_jitStackMgr, tableIdx);
  if (!tab) {
    spdlog::error(WasmEdge::ErrCode::Value::TableOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::TableOutOfBounds);
  }
  const WasmEdge::RefVariant *refP =
      reinterpret_cast<const WasmEdge::RefVariant *>(refPtr);
  if (!tab->setRefAddr(idx, *refP)) {
    spdlog::error(WasmEdge::ErrCode::Value::TableOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::TableOutOfBounds);
  }
}

extern "C" uint32_t jit_table_size(WasmEdge::VM::JitExecEnv *env,
                                   uint32_t tableIdx) {
  (void)env;
  if (!g_jitExecutor || !g_jitStackMgr)
    return 0;
  auto *tab = g_jitExecutor->getTabInstByIdx(*g_jitStackMgr, tableIdx);
  if (!tab)
    return 0;
  return tab->getSize();
}

extern "C" uint32_t jit_table_grow(WasmEdge::VM::JitExecEnv *env,
                                   uint32_t tableIdx, uint32_t n,
                                   const uint64_t *refPtr) {
  (void)env;
  if (!g_jitExecutor || !g_jitStackMgr)
    return static_cast<uint32_t>(-1);
  auto *tab = g_jitExecutor->getTabInstByIdx(*g_jitStackMgr, tableIdx);
  if (!tab)
    return static_cast<uint32_t>(-1);
  WasmEdge::RefVariant ref(tab->getTableType().getRefType());
  if (refPtr) {
    const WasmEdge::RefVariant *refP =
        reinterpret_cast<const WasmEdge::RefVariant *>(refPtr);
    ref = *refP;
  }
  uint32_t oldSize = tab->getSize();
  if (!tab->growTable(n, ref))
    return static_cast<uint32_t>(-1);
  return oldSize;
}

extern "C" void jit_table_fill(WasmEdge::VM::JitExecEnv *env, uint32_t tableIdx,
                               uint32_t offset, uint32_t len,
                               const uint64_t *refPtr) {
  (void)env;
  if (!refPtr || !g_jitExecutor || !g_jitStackMgr)
    return;
  auto *tab = g_jitExecutor->getTabInstByIdx(*g_jitStackMgr, tableIdx);
  if (!tab) {
    spdlog::error(WasmEdge::ErrCode::Value::TableOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::TableOutOfBounds);
  }
  const WasmEdge::RefVariant *refP =
      reinterpret_cast<const WasmEdge::RefVariant *>(refPtr);
  if (!tab->fillRefs(*refP, offset, len)) {
    spdlog::error(WasmEdge::ErrCode::Value::TableOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::TableOutOfBounds);
  }
}

extern "C" void jit_table_copy(WasmEdge::VM::JitExecEnv *env,
                               uint32_t dstTableIdx, uint32_t srcTableIdx,
                               uint32_t dst, uint32_t src, uint32_t len) {
  (void)env;
  if (!g_jitExecutor || !g_jitStackMgr)
    return;
  auto *tabDst = g_jitExecutor->getTabInstByIdx(*g_jitStackMgr, dstTableIdx);
  auto *tabSrc = g_jitExecutor->getTabInstByIdx(*g_jitStackMgr, srcTableIdx);
  if (!tabDst || !tabSrc) {
    spdlog::error(WasmEdge::ErrCode::Value::TableOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::TableOutOfBounds);
  }
  auto srcRefs = tabSrc->getRefs(0, src + len);
  if (!srcRefs) {
    spdlog::error(WasmEdge::ErrCode::Value::TableOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::TableOutOfBounds);
  }
  if (!tabDst->setRefs(*srcRefs, dst, src, len)) {
    spdlog::error(WasmEdge::ErrCode::Value::TableOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::TableOutOfBounds);
  }
}

extern "C" void jit_table_init(WasmEdge::VM::JitExecEnv *env, uint32_t tableIdx,
                               uint32_t elemIdx, uint32_t dst, uint32_t src,
                               uint32_t len) {
  (void)env;
  if (!g_jitExecutor || !g_jitStackMgr)
    return;
  auto *tab = g_jitExecutor->getTabInstByIdx(*g_jitStackMgr, tableIdx);
  auto *elem = g_jitExecutor->getElemInstByIdx(*g_jitStackMgr, elemIdx);
  if (!tab || !elem) {
    spdlog::error(WasmEdge::ErrCode::Value::TableOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::TableOutOfBounds);
  }
  if (!tab->setRefs(elem->getRefs(), dst, src, len)) {
    spdlog::error(WasmEdge::ErrCode::Value::TableOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::TableOutOfBounds);
  }
}

extern "C" void jit_elem_drop(WasmEdge::VM::JitExecEnv *env, uint32_t elemIdx) {
  (void)env;
  if (!g_jitExecutor || !g_jitStackMgr)
    return;
  auto *elem = g_jitExecutor->getElemInstByIdx(*g_jitStackMgr, elemIdx);
  if (elem)
    elem->clear();
}

extern "C" void jit_memory_init(WasmEdge::VM::JitExecEnv *env, uint32_t memIdx,
                                uint32_t dataIdx, uint32_t dst, uint32_t src,
                                uint32_t len) {
  (void)env;
  if (!g_jitExecutor || !g_jitStackMgr)
    return;
  auto *mem = g_jitExecutor->getMemInstByIdx(*g_jitStackMgr, memIdx);
  auto *data = g_jitExecutor->getDataInstByIdx(*g_jitStackMgr, dataIdx);
  if (!mem || !data) {
    spdlog::error(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
    return;
  }
  auto res = mem->setBytes(data->getData(), dst, src, len);
  if (!res) {
    spdlog::error(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
    WasmEdge::Fault::emitFault(WasmEdge::ErrCode::Value::MemoryOutOfBounds);
  }
}

extern "C" void jit_data_drop(WasmEdge::VM::JitExecEnv *env, uint32_t dataIdx) {
  (void)env;
  if (!g_jitExecutor || !g_jitStackMgr)
    return;
  auto *data = g_jitExecutor->getDataInstByIdx(*g_jitStackMgr, dataIdx);
  if (data)
    data->clear();
}
#endif

namespace WasmEdge {
namespace Executor {

#ifdef WASMEDGE_BUILD_IR_JIT
Expect<void> Executor::jitCallFunction(
    Runtime::StackManager &StackMgr,
    const Runtime::Instance::FunctionInstance &Func,
    Span<const ValVariant> Params,
    const Runtime::Instance::ModuleInstance *CallerMod) {
  // Push a dummy frame with the CALLER's module so that host functions
  // (e.g. WASI) get the correct CallingFrame and can access the caller's
  // memory.  runFunction uses nullptr here, which breaks host functions
  // that need memory access when called from JIT code.
  StackMgr.pushFrame(
      const_cast<Runtime::Instance::ModuleInstance *>(CallerMod),
      AST::InstrView::iterator(), 0, 0);

  const auto &PTypes = Func.getFuncType().getParamTypes();
  for (uint32_t I = 0; I < Params.size(); I++) {
    if (PTypes[I].isRefType() && Params[I].get<RefVariant>().getPtr<void>() &&
        Params[I].get<RefVariant>().getType().isNullableRefType()) {
      auto Val = Params[I];
      Val.get<RefVariant>().getType().toNonNullableRef();
      StackMgr.push(Val);
    } else {
      StackMgr.push(Params[I]);
    }
  }

  Expect<void> Res =
      enterFunction(StackMgr, Func, Func.getInstrs().end())
          .and_then([&](AST::InstrView::iterator StartIt) {
            return execute(StackMgr, StartIt, Func.getInstrs().end());
          });

  if (!Res && likely(Res.error() == ErrCode::Value::Terminated)) {
    // When called from JIT (jit_host_call), the stack still has the JIT
    // caller's frame. reset() would wipe it and cause a segfault when
    // returning to the JIT. Only undo what we pushed: host frame + dummy frame.
    size_t nFrames = StackMgr.getFramesSpan().size();
    if (nFrames >= 2) {
      StackMgr.popFrame();
      StackMgr.popFrame();
    } else {
      StackMgr.reset();
    }
  }

  return Res;
}

Expect<uint64_t> Executor::jitCallIndirect(
    Runtime::StackManager &StackMgr, uint32_t TableIdx, uint32_t ElemIdx,
    uint32_t TypeIdx, uint64_t *Args, uint32_t RetTypeCode,
    VM::JitExecEnv *Env) {
  // 1. Get the table instance.
  const auto *TabInst = getTabInstByIdx(StackMgr, TableIdx);
  if (!TabInst || ElemIdx >= TabInst->getSize()) {
    spdlog::error(ErrCode::Value::UndefinedElement);
    return Unexpect(ErrCode::Value::UndefinedElement);
  }

  // 2. Load the funcref at ElemIdx.
  RefVariant Ref = *TabInst->getRefAddr(ElemIdx);
  if (Ref.isNull()) {
    spdlog::error(ErrCode::Value::UninitializedElement);
    return Unexpect(ErrCode::Value::UninitializedElement);
  }

  // 3. Resolve funcref to FunctionInstance.
  const auto *FuncInst = retrieveFuncRef(Ref);
  if (!FuncInst) {
    spdlog::error(ErrCode::Value::UninitializedElement);
    return Unexpect(ErrCode::Value::UninitializedElement);
  }

  // 4. Type-check against the expected type from the call_indirect instruction.
  const auto *ModInst = StackMgr.getModule();
  const auto &ExpDefType = **ModInst->getType(TypeIdx);
  bool IsMatch = false;
  if (FuncInst->getModule()) {
    IsMatch = AST::TypeMatcher::matchType(
        ModInst->getTypeList(), *ExpDefType.getTypeIndex(),
        FuncInst->getModule()->getTypeList(), FuncInst->getTypeIndex());
  } else {
    IsMatch = AST::TypeMatcher::matchType(
        ModInst->getTypeList(), ExpDefType.getCompositeType(),
        FuncInst->getHostFunc().getDefinedType().getCompositeType());
  }
  if (!IsMatch) {
    spdlog::error(ErrCode::Value::IndirectCallTypeMismatch);
    return Unexpect(ErrCode::Value::IndirectCallTypeMismatch);
  }

  // 5. Fast path: if the target is JIT-compiled, call native code directly.
  if (FuncInst->isIRJitFunction()) {
    void *NativeFunc = FuncInst->getIRJitNativeFunc();
    if (NativeFunc) {
      switch (RetTypeCode) {
      case 0: {
        using Fn = void (*)(VM::JitExecEnv *, uint64_t *);
        reinterpret_cast<Fn>(NativeFunc)(Env, Args);
        return static_cast<uint64_t>(0);
      }
      case 1: {
        using Fn = uint64_t (*)(VM::JitExecEnv *, uint64_t *);
        return reinterpret_cast<Fn>(NativeFunc)(Env, Args) & 0xFFFFFFFFu;
      }
      case 2: {
        using Fn = uint64_t (*)(VM::JitExecEnv *, uint64_t *);
        return reinterpret_cast<Fn>(NativeFunc)(Env, Args);
      }
      case 3: {
        using Fn = float (*)(VM::JitExecEnv *, uint64_t *);
        float F = reinterpret_cast<Fn>(NativeFunc)(Env, Args);
        uint64_t U = 0;
        std::memcpy(&U, &F, sizeof(F));
        return U;
      }
      case 4: {
        using Fn = double (*)(VM::JitExecEnv *, uint64_t *);
        double D = reinterpret_cast<Fn>(NativeFunc)(Env, Args);
        uint64_t U = 0;
        std::memcpy(&U, &D, sizeof(D));
        return U;
      }
      default:
        break;
      }
    }
  }

  // 6. Slow path: dispatch through the interpreter.
  const auto &FuncType = FuncInst->getFuncType();
  const auto &ParamTypes = FuncType.getParamTypes();
  const auto &RetTypes = FuncType.getReturnTypes();

  std::vector<ValVariant> Params(ParamTypes.size());
  for (size_t I = 0; I < ParamTypes.size(); ++I) {
    auto Code = ParamTypes[I].getCode();
    if (Code == TypeCode::F32) {
      float F;
      std::memcpy(&F, &Args[I], sizeof(float));
      Params[I] = ValVariant(F);
    } else if (Code == TypeCode::F64) {
      double D;
      std::memcpy(&D, &Args[I], sizeof(double));
      Params[I] = ValVariant(D);
    } else if (Code == TypeCode::I32) {
      Params[I] = ValVariant(static_cast<uint32_t>(Args[I]));
    } else {
      Params[I] = ValVariant(static_cast<uint64_t>(Args[I]));
    }
  }

  auto Res = jitCallFunction(StackMgr, *FuncInst, Params, ModInst);
  if (!Res) {
    return Unexpect(Res.error());
  }

  uint64_t RetVal = 0;
  if (!RetTypes.empty()) {
    auto Val = StackMgr.pop();
    auto Code = RetTypes[0].getCode();
    if (Code == TypeCode::F32) {
      float F = Val.get<float>();
      std::memcpy(&RetVal, &F, sizeof(float));
    } else if (Code == TypeCode::F64) {
      double D = Val.get<double>();
      std::memcpy(&RetVal, &D, sizeof(double));
    } else {
      RetVal = Val.get<uint64_t>();
    }
  }
  return RetVal;
}
#endif

Executor::SavedThreadLocal::SavedThreadLocal(
    Executor &Ex, Runtime::StackManager &StackMgr,
    const Runtime::Instance::FunctionInstance &Func) noexcept {
  // Prepare the execution context.
  auto *ModInst =
      const_cast<Runtime::Instance::ModuleInstance *>(Func.getModule());
  SavedThis = This;
  This = &Ex;

  SavedExecutionContext = ExecutionContext;
  ExecutionContext.StopToken = &Ex.StopToken;
  ExecutionContext.Memories = ModInst->MemoryPtrs.data();
  ExecutionContext.Globals = ModInst->GlobalPtrs.data();
  if (Ex.Stat) {
    ExecutionContext.InstrCount = &Ex.Stat->getInstrCountRef();
    ExecutionContext.CostTable = Ex.Stat->getCostTable().data();
    ExecutionContext.Gas = &Ex.Stat->getTotalCostRef();
    ExecutionContext.GasLimit = Ex.Stat->getCostLimit();
  }

  SavedCurrentStack = CurrentStack;
  CurrentStack = &StackMgr;
}

Executor::SavedThreadLocal::~SavedThreadLocal() noexcept {
  CurrentStack = SavedCurrentStack;
  ExecutionContext = SavedExecutionContext;
  This = SavedThis;
}

Expect<AST::InstrView::iterator>
Executor::enterFunction(Runtime::StackManager &StackMgr,
                        const Runtime::Instance::FunctionInstance &Func,
                        const AST::InstrView::iterator RetIt, bool IsTailCall) {
  // RetIt: the return position when the entered function returns.

  // Check if the interruption occurs.
  if (unlikely(StopToken.exchange(0, std::memory_order_relaxed))) {
    spdlog::error(ErrCode::Value::Interrupted);
    return Unexpect(ErrCode::Value::Interrupted);
  }

  // Get function type for the params and returns num.
  const auto &FuncType = Func.getFuncType();
  const uint32_t ArgsN = static_cast<uint32_t>(FuncType.getParamTypes().size());
  const uint32_t RetsN =
      static_cast<uint32_t>(FuncType.getReturnTypes().size());

  // For the exception handler, remove the inactive handlers caused by the
  // branches.
  if (likely(RetIt)) {
    StackMgr.removeInactiveHandler(RetIt - 1);
  }

  if (Func.isHostFunction()) {
    // Host function case: Push args and call function.
    auto &HostFunc = Func.getHostFunc();

    // Generate CallingFrame from current frame.
    // The module instance will be nullptr if current frame is a dummy frame.
    // For this case, use the module instance of this host function.
    const auto *ModInst = StackMgr.getModule();
    if (ModInst == nullptr) {
      ModInst = Func.getModule();
    }
    Runtime::CallingFrame CallFrame(this, ModInst);

    // Push frame.
    StackMgr.pushFrame(Func.getModule(), // Module instance
                       RetIt,            // Return PC
                       ArgsN,            // Only args, no locals in stack
                       RetsN,            // Returns num
                       IsTailCall        // For tail-call
    );

    // Do the statistics if the statistics turned on.
    if (Stat) {
      // Check host function cost.
      if (unlikely(!Stat->addCost(HostFunc.getCost()))) {
        spdlog::error(ErrCode::Value::CostLimitExceeded);
        return Unexpect(ErrCode::Value::CostLimitExceeded);
      }
      // Start recording time of running host function.
      Stat->stopRecordWasm();
      Stat->startRecordHost();
    }

    // Call pre-host-function
    HostFuncHelper.invokePreHostFunc();

    // Run host function.
    Span<ValVariant> Args = StackMgr.getTopSpan(ArgsN);
    for (uint32_t I = 0; I < ArgsN; I++) {
      // For the number type cases of the arguments, the unused bits should be
      // erased due to the security issue.
      cleanNumericVal(Args[I], FuncType.getParamTypes()[I]);
    }
    std::vector<ValVariant> Rets(RetsN);
    auto Ret = HostFunc.run(CallFrame, std::move(Args), Rets);

    // Call post-host-function
    HostFuncHelper.invokePostHostFunc();

    // Do the statistics if the statistics turned on.
    if (Stat) {
      // Stop recording time of running host function.
      Stat->stopRecordHost();
      Stat->startRecordWasm();
    }

    // Check the host function execution status.
    if (!Ret) {
      if (Ret.error() == ErrCode::Value::HostFuncError ||
          Ret.error().getCategory() != ErrCategory::WASM) {
        spdlog::error(Ret.error());
      }
      return Unexpect(Ret);
    }

    // Push returns back to stack.
    for (auto &R : Rets) {
      StackMgr.push(std::move(R));
    }

    // For host function case, the continuation will be the continuation from
    // the popped frame.
    return StackMgr.popFrame();
  } else if (Func.isCompiledFunction()) {
    // Compiled function case: Execute the function and jump to the
    // continuation.

    // Push frame.
    StackMgr.pushFrame(Func.getModule(), // Module instance
                       RetIt,            // Return PC
                       ArgsN,            // Only args, no locals in stack
                       RetsN,            // Returns num
                       IsTailCall        // For tail-call
    );

    // Prepare arguments.
    Span<ValVariant> Args = StackMgr.getTopSpan(ArgsN);
    std::vector<ValVariant> Rets(RetsN);
    SavedThreadLocal Saved(*this, StackMgr, Func);

    ErrCode Err;
    try {
      // Get symbol and execute the function.
      Fault FaultHandler;
      uint32_t Code = PREPARE_FAULT(FaultHandler);
      if (Code != 0) {
        auto InnerStackTrace = FaultHandler.stacktrace();
        {
          std::array<void *, 256> Buffer;
          auto OuterStackTrace = stackTrace(Buffer);
          while (!OuterStackTrace.empty() && !InnerStackTrace.empty() &&
                 InnerStackTrace[InnerStackTrace.size() - 1] ==
                     OuterStackTrace[OuterStackTrace.size() - 1]) {
            InnerStackTrace = InnerStackTrace.first(InnerStackTrace.size() - 1);
            OuterStackTrace = OuterStackTrace.first(OuterStackTrace.size() - 1);
          }
        }
        StackTraceSize =
            compiledStackTrace(StackMgr, InnerStackTrace, StackTrace).size();
        Err = ErrCode(static_cast<ErrCategory>(Code >> 24), Code);
      } else {
        auto &Wrapper = FuncType.getSymbol();
        Wrapper(&ExecutionContext, Func.getSymbol().get(), Args.data(),
                Rets.data());
      }
    } catch (const ErrCode &E) {
      Err = E;
    }
    if (unlikely(Err)) {
      if (Err != ErrCode::Value::Terminated) {
        spdlog::error(Err);
      }
      StackTraceSize +=
          interpreterStackTrace(
              StackMgr, Span<uint32_t>{StackTrace}.subspan(StackTraceSize))
              .size();
      return Unexpect(Err);
    }

    // Push returns back to stack.
    for (uint32_t I = 0; I < Rets.size(); ++I) {
      StackMgr.push(Rets[I]);
    }

    // For compiled function case, the continuation will be the continuation
    // from the popped frame.
    return StackMgr.popFrame();
  }
#ifdef WASMEDGE_BUILD_IR_JIT
  else if (Func.isIRJitFunction()) {
    // IR JIT compiled function case: Execute the JIT compiled code.

    // Push frame.
    StackMgr.pushFrame(Func.getModule(), // Module instance
                       RetIt,            // Return PC
                       ArgsN,            // Only args, no locals in stack
                       RetsN,            // Returns num
                       IsTailCall        // For tail-call
    );

    // Prepare arguments and returns.
    Span<ValVariant> Args = StackMgr.getTopSpan(ArgsN);
    std::vector<ValVariant> Rets(RetsN);

    // Get module instance for runtime data
    const auto *ModInst = Func.getModule();

    // Get or build cached JIT env (FuncTable, GlobalBase, MemoryBase) for this module
    IRJitEnvCache &Cache = IRJitEnvCache_[ModInst];
    const bool needBuild =
        ModInst &&
        (Cache.FuncTable.size() != ModInst->getFunctionInstances().size() ||
         Cache.GlobalPtrs.size() != ModInst->getGlobalInstances().size());
    if (needBuild) {
      auto FuncInsts = ModInst->getFunctionInstances();
      Cache.FuncTable.resize(FuncInsts.size());
      for (uint32_t I = 0; I < FuncInsts.size(); ++I) {
        const auto *F = FuncInsts[I];
        if (F->isIRJitFunction()) {
          Cache.FuncTable[I] = F->getIRJitNativeFunc();
        } else if (F->isCompiledFunction()) {
          Cache.FuncTable[I] = F->getSymbol().get();
        } else {
          Cache.FuncTable[I] = nullptr;
        }
      }
      auto GlobInsts = ModInst->getGlobalInstances();
      Cache.GlobalPtrs.resize(GlobInsts.size());
      for (size_t I = 0; I < GlobInsts.size(); ++I) {
        Cache.GlobalPtrs[I] =
            const_cast<ValVariant *>(&GlobInsts[I]->getValue());
      }
      auto MemInsts = ModInst->getMemoryInstances();
      Cache.MemoryBase =
          (!MemInsts.empty() && MemInsts[0]) ? MemInsts[0]->getDataPtr() : nullptr;
    }
    void **FuncTable =
        Cache.FuncTable.empty() ? nullptr : Cache.FuncTable.data();
    uint32_t FuncTableSize =
        static_cast<uint32_t>(Cache.FuncTable.size());
    void *GlobalBase =
        Cache.GlobalPtrs.empty() ? nullptr : Cache.GlobalPtrs.data();
    void *MemoryBase = Cache.MemoryBase;

    // Use the Executor's single IR JIT engine
    VM::IRJitEngine &IREngine = getIRJitEngine();

    // Set up TLS context for jit_host_call trampoline
    g_jitExecutor = this;
    g_jitStackMgr = &StackMgr;
    g_jitModInst = ModInst;
    // Cache memory instance for JIT helpers; table 0 is resolved via getTabInstByIdx in jit_host_call.
    if (ModInst) {
      auto memRes = ModInst->getMemory(0);
      g_jitMemory0 = memRes ? *memRes : nullptr;
    } else {
      g_jitMemory0 = nullptr;
    }

    auto Res = IREngine.invoke(Func.getIRJitNativeFunc(), FuncType, Args, Rets,
                               FuncTable, FuncTableSize, GlobalBase,
                               MemoryBase);

    if (!Res) {
      if (Res.error() != ErrCode::Value::Terminated) {
        spdlog::error(Res.error());
      }
      return Unexpect(Res);
    }

    // Push returns back to stack.
    for (uint32_t I = 0; I < Rets.size(); ++I) {
      StackMgr.push(Rets[I]);
    }

    // For IR JIT function case, the continuation will be the continuation from
    // the popped frame.
    return StackMgr.popFrame();
  }
#endif
  else {
    // Native function case: Jump to the start of the function body.

    // Push local variables into the stack.
    for (auto &Def : Func.getLocals()) {
      for (uint32_t I = 0; I < Def.first; I++) {
        StackMgr.push(ValueFromType(Def.second));
      }
    }

    // Push frame.
    // The PC must -1 here because in the interpreter mode execution, the PC
    // will increase after the callee return.
    StackMgr.pushFrame(Func.getModule(),           // Module instance
                       RetIt - 1,                  // Return PC
                       ArgsN + Func.getLocalNum(), // Arguments num + local num
                       RetsN,                      // Returns num
                       IsTailCall                  // For tail-call
    );

    // For native function case, the continuation will be the start of the
    // function body.
    return Func.getInstrs().begin();
  }
}

Expect<void>
Executor::branchToLabel(Runtime::StackManager &StackMgr,
                        const AST::Instruction::JumpDescriptor &JumpDesc,
                        AST::InstrView::iterator &PC) noexcept {
  // Check the stop token.
  if (unlikely(StopToken.exchange(0, std::memory_order_relaxed))) {
    spdlog::error(ErrCode::Value::Interrupted);
    return Unexpect(ErrCode::Value::Interrupted);
  }

  StackMgr.eraseValueStack(JumpDesc.StackEraseBegin, JumpDesc.StackEraseEnd);
  // PC need to -1 here because the PC will increase in the next iteration.
  PC += (JumpDesc.PCOffset - 1);
  return {};
}

Expect<void> Executor::throwException(Runtime::StackManager &StackMgr,
                                      Runtime::Instance::TagInstance &TagInst,
                                      AST::InstrView::iterator &PC) noexcept {
  StackMgr.removeInactiveHandler(PC);
  auto AssocValSize = TagInst.getTagType().getAssocValSize();
  while (true) {
    // Pop the top handler.
    auto Handler = StackMgr.popTopHandler(AssocValSize);
    if (!Handler.has_value()) {
      break;
    }
    // Checking through the catch clause.
    for (const auto &C : Handler->CatchClause) {
      if (!C.IsAll && getTagInstByIdx(StackMgr, C.TagIndex) != &TagInst) {
        // For catching a specific tag, should check the equivalence of tag
        // address.
        continue;
      }
      if (C.IsRef) {
        // For catching a exception reference, push the reference value onto
        // stack.
        StackMgr.push(
            RefVariant(ValType(TypeCode::Ref, TypeCode::ExnRef), &TagInst));
      }
      // When being here, an exception is caught. Move the PC to the try block
      // and branch to the label.

      PC = Handler->Try;
      return branchToLabel(StackMgr, C.Jump, PC);
    }
  }
  spdlog::error(ErrCode::Value::UncaughtException);
  return Unexpect(ErrCode::Value::UncaughtException);
}

const AST::SubType *Executor::getDefTypeByIdx(Runtime::StackManager &StackMgr,
                                              const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetType(Idx);
}

const WasmEdge::AST::CompositeType &
Executor::getCompositeTypeByIdx(Runtime::StackManager &StackMgr,
                                const uint32_t Idx) const noexcept {
  auto *DefType = getDefTypeByIdx(StackMgr, Idx);
  assuming(DefType);
  const auto &CompType = DefType->getCompositeType();
  assuming(!CompType.isFunc());
  return CompType;
}

const ValType &
Executor::getStructStorageTypeByIdx(Runtime::StackManager &StackMgr,
                                    const uint32_t Idx,
                                    const uint32_t Off) const noexcept {
  const auto &CompType = getCompositeTypeByIdx(StackMgr, Idx);
  assuming(static_cast<uint32_t>(CompType.getFieldTypes().size()) > Off);
  return CompType.getFieldTypes()[Off].getStorageType();
}

const ValType &
Executor::getArrayStorageTypeByIdx(Runtime::StackManager &StackMgr,
                                   const uint32_t Idx) const noexcept {
  const auto &CompType = getCompositeTypeByIdx(StackMgr, Idx);
  assuming(static_cast<uint32_t>(CompType.getFieldTypes().size()) == 1);
  return CompType.getFieldTypes()[0].getStorageType();
}

Runtime::Instance::FunctionInstance *
Executor::getFuncInstByIdx(Runtime::StackManager &StackMgr,
                           const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetFunction(Idx);
}

Runtime::Instance::TableInstance *
Executor::getTabInstByIdx(Runtime::StackManager &StackMgr,
                          const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetTable(Idx);
}

Runtime::Instance::MemoryInstance *
Executor::getMemInstByIdx(Runtime::StackManager &StackMgr,
                          const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetMemory(Idx);
}

Runtime::Instance::TagInstance *
Executor::getTagInstByIdx(Runtime::StackManager &StackMgr,
                          const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetTag(Idx);
}

Runtime::Instance::GlobalInstance *
Executor::getGlobInstByIdx(Runtime::StackManager &StackMgr,
                           const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetGlobal(Idx);
}

Runtime::Instance::ElementInstance *
Executor::getElemInstByIdx(Runtime::StackManager &StackMgr,
                           const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetElem(Idx);
}

Runtime::Instance::DataInstance *
Executor::getDataInstByIdx(Runtime::StackManager &StackMgr,
                           const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetData(Idx);
}

TypeCode Executor::toBottomType(Runtime::StackManager &StackMgr,
                                const ValType &Type) const {
  if (Type.isRefType()) {
    if (Type.isAbsHeapType()) {
      switch (Type.getHeapTypeCode()) {
      case TypeCode::NullFuncRef:
      case TypeCode::FuncRef:
        return TypeCode::NullFuncRef;
      case TypeCode::NullExternRef:
      case TypeCode::ExternRef:
        return TypeCode::NullExternRef;
      case TypeCode::NullRef:
      case TypeCode::AnyRef:
      case TypeCode::EqRef:
      case TypeCode::I31Ref:
      case TypeCode::StructRef:
      case TypeCode::ArrayRef:
        return TypeCode::NullRef;
      case TypeCode::NullExnRef:
      case TypeCode::ExnRef:
        return TypeCode::NullExnRef;
      default:
        assumingUnreachable();
      }
    } else {
      const auto &CompType =
          (*StackMgr.getModule()->getType(Type.getTypeIndex()))
              ->getCompositeType();
      if (CompType.isFunc()) {
        return TypeCode::NullFuncRef;
      } else {
        return TypeCode::NullRef;
      }
    }
  } else {
    return Type.getCode();
  }
}

void Executor::cleanNumericVal(ValVariant &Val,
                               const ValType &Type) const noexcept {
  if (Type.isNumType()) {
    switch (Type.getCode()) {
    case TypeCode::I32: {
      uint32_t V = Val.get<uint32_t>();
      Val.emplace<uint128_t>(static_cast<uint128_t>(0U));
      Val.emplace<uint32_t>(V);
      break;
    }
    case TypeCode::F32: {
      float V = Val.get<float>();
      Val.emplace<uint128_t>(static_cast<uint128_t>(0U));
      Val.emplace<float>(V);
      break;
    }
    case TypeCode::I64: {
      uint64_t V = Val.get<uint64_t>();
      Val.emplace<uint128_t>(static_cast<uint128_t>(0U));
      Val.emplace<uint64_t>(V);
      break;
    }
    case TypeCode::F64: {
      double V = Val.get<double>();
      Val.emplace<uint128_t>(static_cast<uint128_t>(0U));
      Val.emplace<double>(V);
      break;
    }
    default:
      break;
    }
  }
}

ValVariant Executor::packVal(const ValType &Type,
                             const ValVariant &Val) const noexcept {
  if (Type.isPackType()) {
    switch (Type.getCode()) {
    case TypeCode::I8:
      if constexpr (Endian::native == Endian::little) {
        return ValVariant(Val.get<uint32_t>() & 0xFFU);
      } else {
        return ValVariant(Val.get<uint32_t>() << 24);
      }
    case TypeCode::I16:
      if constexpr (Endian::native == Endian::little) {
        return ValVariant(Val.get<uint32_t>() & 0xFFFFU);
      } else {
        return ValVariant(Val.get<uint32_t>() << 16);
      }
    default:
      assumingUnreachable();
    }
  }
  return Val;
}

std::vector<ValVariant>
Executor::packVals(const ValType &Type,
                   std::vector<ValVariant> &&Vals) const noexcept {
  for (uint32_t I = 0; I < Vals.size(); I++) {
    Vals[I] = packVal(Type, Vals[I]);
  }
  return std::move(Vals);
}

ValVariant Executor::unpackVal(const ValType &Type, const ValVariant &Val,
                               bool IsSigned) const noexcept {
  if (Type.isPackType()) {
    uint32_t Num = Val.get<uint32_t>();
    switch (Type.getCode()) {
    case TypeCode::I8:
      if constexpr (Endian::native == Endian::big) {
        Num >>= 24;
      }
      if (IsSigned) {
        return static_cast<uint32_t>(static_cast<int8_t>(Num));
      } else {
        return static_cast<uint32_t>(static_cast<uint8_t>(Num));
      }
    case TypeCode::I16:
      if constexpr (Endian::native == Endian::big) {
        Num >>= 16;
      }
      if (IsSigned) {
        return static_cast<uint32_t>(static_cast<int16_t>(Num));
      } else {
        return static_cast<uint32_t>(static_cast<uint16_t>(Num));
      }
    default:
      assumingUnreachable();
    }
  }
  return Val;
}
} // namespace Executor
} // namespace WasmEdge
