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
/// (WASMEDGE_IR_JIT_OPT_LEVEL=2) when using this convention.
struct JitExecEnv {
  void **FuncTable;
  uint32_t FuncTableSize;
  uint32_t _pad;
  void *GlobalBase;
  void *MemoryBase;
  void *HostCallFn;      // Pointer to jit_host_call trampoline (extern "C")
  void *DirectOrHostFn; // Pointer to jit_direct_or_host (null-safe direct call)
  void *MemoryGrowFn;   // Pointer to jit_memory_grow trampoline
  void *MemorySizeFn;   // Pointer to jit_memory_size trampoline
  void *CallIndirectFn; // Pointer to jit_call_indirect trampoline
  uint64_t MemorySizeBytes; // Current linear memory size in bytes (for bounds checking)
  /// 16-byte buffer for ref-return helpers (jit_ref_func, jit_table_get).
  uint64_t RefResultBuf[2];
};

/// Host call trampoline: dispatches calls to non-JIT functions (imports)
/// through the WasmEdge executor.  For direct `call` instructions to imports.
/// For `call_indirect`, the funcIdx encodes the table slot with bit 31 set:
/// pass (0x80000000 | tableSlot) as funcIdx.
extern "C" uint64_t jit_host_call(JitExecEnv *env, uint32_t funcIdx,
                                  uint64_t *args);
/// Null-safe direct call: if funcPtr is null, dispatches via jit_host_call.
/// retTypeCode: 0=void, 1=i32, 2=i64, 3=f32, 4=f64
extern "C" uint64_t jit_direct_or_host(JitExecEnv *env, void *funcPtr,
                                        uint32_t funcIdx, uint64_t *args,
                                        uint32_t retTypeCode);
/// JMP buf for unwinding on proc_exit (Terminated). Used by jit_host_call to
/// longjmp back to invoke() so we do not return to JIT and run unreachable.
extern "C" void *wasmedge_ir_jit_get_termination_buf(void);
/// OOB trap handler: longjmps back to invoke() with value 2 (→ MemoryOutOfBounds).
extern "C" void jit_oob_trap(void);
/// Wasm linear-memory bounds: ea = base+offset (i32 wrap); trap if zext(ea)+access_size > MemorySizeBytes.
/// Outlined so IR JIT does not emit IF/UNREACHABLE per access (avoids ir_reg_alloc hang at O1+).
extern "C" void jit_bounds_check(JitExecEnv *env, uint32_t base, uint32_t offset,
                                 uint32_t access_size);
/// call_indirect trampoline: resolves table[tableIdx][elemIdx], type-checks
/// against typeIdx, then dispatches to JIT native code or interpreter.
/// retTypeCode: 0=void, 1=i32, 2=i64, 3=f32, 4=f64
extern "C" uint64_t jit_call_indirect(JitExecEnv *env, uint32_t tableIdx,
                                       uint32_t elemIdx, uint32_t typeIdx,
                                       uint64_t *args, uint32_t retTypeCode);
/// memory.grow trampoline: grows memory by N pages, returns old size or -1.
/// Also updates env->MemoryBase to the (potentially relocated) data pointer.
extern "C" int32_t jit_memory_grow(JitExecEnv *env, uint32_t nPages);
/// memory.size trampoline: returns current memory size in pages.
extern "C" int32_t jit_memory_size(JitExecEnv *env);
/// ref.func helper: writes RefVariant for function at funcIdx to env->RefResultBuf.
extern "C" void jit_ref_func(JitExecEnv *env, uint32_t funcIdx);
/// table.get: writes table[tableIdx][idx] to env->RefResultBuf. Traps on OOB.
extern "C" void jit_table_get(JitExecEnv *env, uint32_t tableIdx, uint32_t idx);
/// table.set: reads RefVariant from refPtr, sets table[tableIdx][idx]. Traps on OOB.
extern "C" void jit_table_set(JitExecEnv *env, uint32_t tableIdx, uint32_t idx,
                              const uint64_t *refPtr);
/// table.size: returns current size of table[tableIdx].
extern "C" uint32_t jit_table_size(JitExecEnv *env, uint32_t tableIdx);
/// table.grow: grows table by n, init with ref at refPtr. Returns old size or (uint32_t)-1.
extern "C" uint32_t jit_table_grow(JitExecEnv *env, uint32_t tableIdx,
                                   uint32_t n, const uint64_t *refPtr);
/// table.fill: fills table[tableIdx][offset..offset+len-1] with ref at refPtr. Traps on OOB.
extern "C" void jit_table_fill(JitExecEnv *env, uint32_t tableIdx,
                               uint32_t offset, uint32_t len, const uint64_t *refPtr);
/// table.copy: copies len refs from srcTable[src] to dstTable[dst]. Traps on OOB.
extern "C" void jit_table_copy(JitExecEnv *env, uint32_t dstTableIdx,
                               uint32_t srcTableIdx, uint32_t dst, uint32_t src,
                               uint32_t len);
/// table.init: copies len refs from elem[elemIdx][src] to table[tableIdx][dst]. Traps on OOB.
extern "C" void jit_table_init(JitExecEnv *env, uint32_t tableIdx, uint32_t elemIdx,
                               uint32_t dst, uint32_t src, uint32_t len);
/// elem.drop: clears element segment elemIdx.
extern "C" void jit_elem_drop(JitExecEnv *env, uint32_t elemIdx);
/// memory.copy: copies len bytes from srcMem[src] to dstMem[dst]. Traps on OOB.
extern "C" void jit_memory_copy(JitExecEnv *env, uint32_t dstMemIdx,
                                uint32_t srcMemIdx, uint32_t dst, uint32_t src,
                                uint32_t len);
/// memory.fill: fills mem[off..off+len-1] with byte val. Traps on OOB.
extern "C" void jit_memory_fill(JitExecEnv *env, uint32_t memIdx, uint32_t off,
                                uint32_t val, uint32_t len);
/// memory.init: copies len bytes from data[dataIdx][src] to mem[memIdx][dst]. Traps on OOB.
extern "C" void jit_memory_init(JitExecEnv *env, uint32_t memIdx,
                                uint32_t dataIdx, uint32_t dst, uint32_t src,
                                uint32_t len);
/// data.drop: clears data segment dataIdx.
extern "C" void jit_data_drop(JitExecEnv *env, uint32_t dataIdx);

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
                      void *MemoryBase = nullptr, uint64_t MemorySize = 0);

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

  /// Convert ValVariant to raw 64-bit value (type-aware; preserves F32/F64 bits)
  uint64_t valVariantToRaw(const ValVariant &Val, ValType Type) const noexcept;

  /// Convert raw value to ValVariant
  ValVariant rawToValVariant(uint64_t Raw, ValType Type) const noexcept;

private:
  std::vector<CodeBuffer> CodeBuffers; // Track allocated code buffers
  /// Reusable buffer for marshalling args in invoke() (avoids per-call allocation)
  mutable std::vector<uint64_t> ArgsBuffer_;
};

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT

