// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/ir_jit_reg_invoke.h"

#ifdef WASMEDGE_BUILD_IR_JIT

#include "ast/type.h"
#include "common/span.h"
#include "common/spdlog.h"
#include <cstring>

namespace WasmEdge {
namespace VM {
namespace detail {

uint64_t irJitInvokeNativeAutogen(void *Fn, JitExecEnv *Env,
                                  Span<const ValType> ParamTypes,
                                  const uint64_t *Slots, ValType RetType);
void irJitInvokeNativeAutogenVoid(void *Fn, JitExecEnv *Env,
                                  Span<const ValType> ParamTypes,
                                  const uint64_t *Slots);

uint64_t irJitInvokeNative(void *NativeFunc, JitExecEnv *Env,
                           const AST::FunctionType &FuncType,
                           const uint64_t *ArgSlots) {
  const auto &ParamTypes = FuncType.getParamTypes();
  if (ParamTypes.size() > 6) {
    spdlog::error(
        "IR JIT register invoke: arity {} exceeds compiled dispatch limit (6)",
        ParamTypes.size());
    std::abort();
  }
  Span<const ValType> PSpan(ParamTypes.data(), ParamTypes.size());
  const auto &RetTypes = FuncType.getReturnTypes();
  if (RetTypes.empty()) {
    irJitInvokeNativeAutogenVoid(NativeFunc, Env, PSpan, ArgSlots);
    return 0;
  }
  return irJitInvokeNativeAutogen(NativeFunc, Env, PSpan, ArgSlots, RetTypes[0]);
}

uint64_t irJitInvokeNativeBuffer(void *NativeFunc, JitExecEnv *Env,
                                 const AST::FunctionType &FuncType,
                                 uint64_t *ArgSlots) {
  // Buffer-based ABI: func(JitExecEnv*, uint64_t* args)
  // Used for functions with >kRegCallMaxParams wasm parameters.
  const auto &RetTypes = FuncType.getReturnTypes();
  if (RetTypes.empty()) {
    reinterpret_cast<void (*)(JitExecEnv *, uint64_t *)>(NativeFunc)(Env,
                                                                      ArgSlots);
    return 0;
  }
  auto Code = RetTypes[0].getCode();
  if (Code == TypeCode::F32) {
    float F = reinterpret_cast<float (*)(JitExecEnv *, uint64_t *)>(
        NativeFunc)(Env, ArgSlots);
    uint64_t Raw = 0;
    std::memcpy(&Raw, &F, sizeof(F));
    return Raw;
  }
  if (Code == TypeCode::F64) {
    double D = reinterpret_cast<double (*)(JitExecEnv *, uint64_t *)>(
        NativeFunc)(Env, ArgSlots);
    uint64_t Raw = 0;
    std::memcpy(&Raw, &D, sizeof(D));
    return Raw;
  }
  return reinterpret_cast<uint64_t (*)(JitExecEnv *, uint64_t *)>(NativeFunc)(
      Env, ArgSlots);
}

} // namespace detail
} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT
