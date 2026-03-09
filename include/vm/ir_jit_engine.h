// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/vm/ir_jit_engine.h - IR JIT Engine class definition ------===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the declaration of the IR JIT Engine class, which
/// compiles and executes WebAssembly functions using the dstogov/ir framework.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "common/errcode.h"
#include "common/span.h"
#include "common/types.h"

#ifdef WASMEDGE_BUILD_IR_JIT

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Forward declare IR types
struct _ir_ctx;
typedef struct _ir_ctx ir_ctx;

namespace WasmEdge {

namespace AST {
class FunctionType;
} // namespace AST

namespace Runtime {
class CallingFrame;
} // namespace Runtime

namespace VM {

class WasmToIRBuilder;

/// Execution environment passed as the first argument to every JIT-compiled
/// function. Used with the uniform signature ret func(JitExecEnv*, uint64_t*).
/// O0 code emitter has a bug fusing LOAD(addr)->ADDR with ADD; use O2
/// (WASMEDGE_IR_JIT_OPT_LEVEL=2) when using this convention.
struct JitExecEnv {
  void **FuncTable;
  uint32_t FuncTableSize;
  uint32_t _pad;
  void *GlobalBase;
  void *MemoryBase;
  void *HostCallFn;   // Pointer to jit_host_call trampoline (extern "C")
};

/// Host call trampoline: dispatches calls to non-JIT functions (imports)
/// through the WasmEdge executor.  For direct `call` instructions to imports.
/// For `call_indirect`, the funcIdx encodes the table slot with bit 31 set:
/// pass (0x80000000 | tableSlot) as funcIdx.
extern "C" uint64_t jit_host_call(JitExecEnv *env, uint32_t funcIdx,
                                  uint64_t *args);

/// IR JIT Engine - compiles and executes IR code
class IRJitEngine {
public:
  /// Compilation result
  struct CompileResult {
    void *NativeFunc;     // Pointer to generated native code
    size_t CodeSize;      // Size of generated code in bytes
    ir_ctx *IRGraph;      // Preserved IR graph for potential tier-up
  };

  IRJitEngine() noexcept;
  ~IRJitEngine() noexcept;

  // Disable copy
  IRJitEngine(const IRJitEngine &) = delete;
  IRJitEngine &operator=(const IRJitEngine &) = delete;

  /// Compile a function from IR context
  Expect<CompileResult> compile(ir_ctx *Ctx);

  /// Execute a compiled function.
  /// Uniform calling convention: ret func(JitExecEnv* env, uint64_t* args)
  Expect<void> invoke(void *NativeFunc, const AST::FunctionType &FuncType,
                      Span<const ValVariant> Args, Span<ValVariant> Rets,
                      void **FuncTable = nullptr, uint32_t FuncTableSize = 0,
                      void *GlobalBase = nullptr,
                      void *MemoryBase = nullptr, uint32_t MemorySize = 0);

  /// Release compiled code
  void release(void *NativeFunc, size_t CodeSize) noexcept;

  /// Release IR graph
  void releaseIRGraph(ir_ctx *Ctx) noexcept;

private:
  /// Code buffer management
  struct CodeBuffer {
    void *Code;
    size_t Size;
    size_t Capacity;
  };

  /// Allocate executable memory
  void *allocateExecutable(size_t Size);

  /// Free executable memory
  void freeExecutable(void *Ptr, size_t Size) noexcept;

  /// Convert ValVariant to raw value
  uint64_t valVariantToRaw(const ValVariant &Val) const noexcept;

  /// Convert raw value to ValVariant
  ValVariant rawToValVariant(uint64_t Raw, ValType Type) const noexcept;

private:
  std::vector<CodeBuffer> CodeBuffers; // Track allocated code buffers
};

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT

