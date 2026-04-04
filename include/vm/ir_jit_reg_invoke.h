// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/vm/ir_jit_reg_invoke.h - IR JIT SysV register invoke -------===//
//
// Invokes a native IR-JIT function pointer using the same parameter layout as
// ir_PARAM in WasmToIRBuilder::initialize (JitExecEnv* then Wasm params).
//
//===----------------------------------------------------------------------===//
#pragma once

#include "common/span.h"
#include "common/types.h"

#ifdef WASMEDGE_BUILD_IR_JIT

namespace WasmEdge {
namespace AST {
class FunctionType;
}
namespace VM {

struct JitExecEnv;

/// Maximum wasm parameter count for register-based calling convention.
/// Functions with ≤kRegCallMaxParams wasm args use SysV register ABI;
/// functions with more use buffer-based: func(env, uint64_t *args).
/// With 3 wasm params: env + 3 params = 4 callee-saved regs, leaving 2 free.
static constexpr uint32_t kRegCallMaxParams = 0;

namespace detail {

/// Call a JIT-compiled Wasm function with register ABI (≤kRegCallMaxParams).
/// Returns raw bits in rax (i32/i64/f32/f64 as uint64_t); void returns 0.
uint64_t irJitInvokeNative(void *NativeFunc, JitExecEnv *Env,
                           const AST::FunctionType &FuncType,
                           const uint64_t *ArgSlots);

/// Call a JIT-compiled Wasm function with buffer ABI (>kRegCallMaxParams).
/// Signature: ret func(JitExecEnv*, uint64_t* args).
/// Returns raw bits; void returns 0.
uint64_t irJitInvokeNativeBuffer(void *NativeFunc, JitExecEnv *Env,
                                 const AST::FunctionType &FuncType,
                                 uint64_t *ArgSlots);

} // namespace detail
} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT
