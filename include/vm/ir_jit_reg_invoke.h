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

namespace detail {

/// Call a JIT-compiled Wasm function with register ABI. Returns raw bits in
/// rax (i32/i64/f32/f64 as uint64_t per IR JIT convention); void functions
/// return 0.
uint64_t irJitInvokeNative(void *NativeFunc, JitExecEnv *Env,
                           const AST::FunctionType &FuncType,
                           const uint64_t *ArgSlots);

} // namespace detail
} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT
