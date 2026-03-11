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
#include <chrono>
#include <fstream>
#endif

#include <cstdint>
#include <utility>
#include <vector>

#ifdef WASMEDGE_BUILD_IR_JIT
static thread_local WasmEdge::Executor::Executor *g_jitExecutor = nullptr;
static thread_local WasmEdge::Runtime::StackManager *g_jitStackMgr = nullptr;
static thread_local const WasmEdge::Runtime::Instance::ModuleInstance *g_jitModInst = nullptr;
static thread_local WasmEdge::Runtime::Instance::TableInstance *g_jitTable0 = nullptr;
static thread_local WasmEdge::Runtime::Instance::MemoryInstance *g_jitMemory0 = nullptr;

extern "C" uint64_t jit_host_call(WasmEdge::VM::JitExecEnv *env,
                                  uint32_t funcIdx, uint64_t *args) {
  // #region agent log
  {
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char buf[384];
    std::snprintf(buf, sizeof(buf),
      "{\"sessionId\":\"d32b78\",\"location\":\"helper.cpp:jit_host_call\",\"message\":\"entry\",\"data\":{\"env\":\"%p\",\"funcIdx\":%u,\"HostCallFn\":\"%p\",\"g_jitExecutor\":\"%p\",\"g_jitModInst\":\"%p\"},\"runId\":\"run1\",\"hypothesisId\":\"H3,H4\",\"timestamp\":%lld}\n",
      (void*)env, funcIdx, env ? env->HostCallFn : (void*)0, (void*)g_jitExecutor, (void*)g_jitModInst, (long long)ts);
    std::ofstream f("/home/tommy/Desktop/wasmedge/.cursor/debug-d32b78.log", std::ios::app); if (f) f << buf; f.close();
  }
  // #endregion
  (void)env;
  if (!g_jitExecutor || !g_jitStackMgr || !g_jitModInst)
    return 0;

  const WasmEdge::Runtime::Instance::FunctionInstance *funcInst = nullptr;

  if (funcIdx & 0x80000000u) {
    // call_indirect: bit 31 set, lower bits = table slot index
    uint32_t tableSlot = funcIdx & 0x7FFFFFFFu;
    if (!g_jitTable0)
      return 0;
    auto refRes = g_jitTable0->getRefAddr(tableSlot);
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
  // #region agent log
  {
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char buf[192];
    std::snprintf(buf, sizeof(buf),
      "{\"sessionId\":\"d32b78\",\"location\":\"helper.cpp:jit_host_call_exit\",\"message\":\"return\",\"data\":{\"funcIdx\":%u},\"runId\":\"run1\",\"hypothesisId\":\"H6\",\"timestamp\":%lld}\n",
      funcIdx, (long long)ts);
    std::ofstream f("/home/tommy/Desktop/wasmedge/.cursor/debug-d32b78.log", std::ios::app); if (f) f << buf; f.close();
  }
  // #endregion
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
#endif

namespace WasmEdge {
namespace Executor {

#ifdef WASMEDGE_BUILD_IR_JIT
Expect<void> Executor::jitCallFunction(
    Runtime::StackManager &StackMgr,
    const Runtime::Instance::FunctionInstance &Func,
    Span<const ValVariant> Params,
    const Runtime::Instance::ModuleInstance *CallerMod) {
  // #region agent log
  {
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char buf[256];
    std::snprintf(buf, sizeof(buf),
      "{\"sessionId\":\"d32b78\",\"location\":\"helper.cpp:jitCallFunction\",\"message\":\"entry\",\"data\":{\"CallerMod\":\"%p\",\"StackMgrFrames\":%zu},\"runId\":\"run1\",\"hypothesisId\":\"H7\",\"timestamp\":%lld}\n",
      (void*)CallerMod, StackMgr.getFramesSpan().size(), (long long)ts);
    std::ofstream f("/home/tommy/Desktop/wasmedge/.cursor/debug-d32b78.log", std::ios::app); if (f) f << buf; f.close();
  }
  // #endregion
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

  // #region agent log
  {
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char buf[320];
    std::snprintf(buf, sizeof(buf),
      "{\"sessionId\":\"d32b78\",\"location\":\"helper.cpp:jitCallFunction\",\"message\":\"pre_return\",\"data\":{\"Res\":%s,\"Terminated\":%d},\"runId\":\"run1\",\"hypothesisId\":\"H7\",\"timestamp\":%lld}\n",
      Res ? "ok" : "err", (!Res && Res.error() == ErrCode::Value::Terminated) ? 1 : 0, (long long)ts);
    std::ofstream f("/home/tommy/Desktop/wasmedge/.cursor/debug-d32b78.log", std::ios::app); if (f) f << buf; f.close();
  }
  // #endregion
  if (!Res && likely(Res.error() == ErrCode::Value::Terminated)) {
    // When called from JIT (jit_host_call), the stack still has the JIT
    // caller's frame. reset() would wipe it and cause a segfault when
    // returning to the JIT. Only undo what we pushed: host frame + dummy frame.
    size_t nFrames = StackMgr.getFramesSpan().size();
    if (nFrames >= 2) {
      StackMgr.popFrame();
      StackMgr.popFrame();
      // #region agent log
      {
        auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        char buf[256];
        std::snprintf(buf, sizeof(buf),
          "{\"sessionId\":\"d32b78\",\"location\":\"helper.cpp:jitCallFunction\",\"message\":\"after_pop2\",\"data\":{\"nFramesBefore\":%zu},\"runId\":\"run1\",\"hypothesisId\":\"H7\",\"timestamp\":%lld}\n",
          nFrames, (long long)ts);
        std::ofstream f("/home/tommy/Desktop/wasmedge/.cursor/debug-d32b78.log", std::ios::app); if (f) f << buf; f.close();
      }
      // #endregion
    } else {
      StackMgr.reset();
    }
  }

  return Res;
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
    // #region agent log
    {
      auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      char buf[512];
      std::snprintf(buf, sizeof(buf),
        "{\"sessionId\":\"d32b78\",\"location\":\"helper.cpp:pre_invoke\",\"message\":\"cache_and_env\",\"data\":{\"FuncTable\":\"%p\",\"FuncTableSize\":%u,\"Ft0\":\"%p\",\"Ft1\":\"%p\",\"NativeFunc\":\"%p\",\"g_jitModInst\":\"%p\"},\"runId\":\"run1\",\"hypothesisId\":\"H1,H5\",\"timestamp\":%lld}\n",
        (void*)FuncTable, (unsigned)FuncTableSize,
        FuncTable && FuncTableSize > 0 ? (void*)FuncTable[0] : (void*)0,
        FuncTable && FuncTableSize > 1 ? (void*)FuncTable[1] : (void*)0,
        (void*)Func.getIRJitNativeFunc(), (void*)g_jitModInst, (long long)ts);
      std::ofstream f("/home/tommy/Desktop/wasmedge/.cursor/debug-d32b78.log", std::ios::app); if (f) f << buf; f.close();
    }
    // #endregion
    // Modules without a table (e.g. noop) have empty TabInsts; getTabInstByIdx
    // would UB. Use safe getTable and set null when no table.
    if (ModInst) {
      auto tabRes = ModInst->getTable(0);
      g_jitTable0 = tabRes ? *tabRes : nullptr;
      auto memRes = ModInst->getMemory(0);
      g_jitMemory0 = memRes ? *memRes : nullptr;
    } else {
      g_jitTable0 = nullptr;
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
