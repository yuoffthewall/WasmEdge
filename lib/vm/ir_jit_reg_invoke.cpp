// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/ir_jit_reg_invoke.h"

#ifdef WASMEDGE_BUILD_IR_JIT

#include "ast/type.h"
#include "common/span.h"
#include "common/spdlog.h"

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

} // namespace detail
} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT
