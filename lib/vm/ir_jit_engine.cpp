// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/ir_jit_engine.h"

#ifdef WASMEDGE_BUILD_IR_JIT

#include "ast/type.h"
#include "common/errcode.h"
#include "vm/ir_builder.h"
#include <spdlog/spdlog.h>

// Include dstogov/ir headers
extern "C" {
#include "ir.h"
}

#include <cstring>
#include <sys/mman.h>

namespace WasmEdge {
namespace VM {

IRJitEngine::IRJitEngine() noexcept {}

IRJitEngine::~IRJitEngine() noexcept {
  // Release all code buffers
  for (auto &Buffer : CodeBuffers) {
    freeExecutable(Buffer.Code, Buffer.Size);
  }
  CodeBuffers.clear();
}

Expect<IRJitEngine::CompileResult>
IRJitEngine::compile(ir_ctx *Ctx) {
  if (!Ctx) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // Verify IR before compilation to catch invalid patterns that would crash
  // the backend. With release build of IR library, this returns false instead
  // of aborting on invalid IR.
  if (!ir_check(Ctx)) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // Use IR's built-in JIT compilation
  // Optimization level: 0 = no opt, 1 = light, 2 = full
  // Using level 0 for now to avoid GCM assertion failures in IR backend
  size_t CodeSize = 0;
  void *NativeCode = ir_jit_compile(Ctx, 0, &CodeSize);
  
  if (!NativeCode) {
    spdlog::info("IR JIT: ir_jit_compile failed");
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // Track the code buffer
  CodeBuffers.push_back({NativeCode, CodeSize, CodeSize});

  // Register with GDB JIT interface so breakpoints / disassembly work
  ir_gdb_register("wasm_jit_func", NativeCode, CodeSize, 0, 0);

  CompileResult Result;
  Result.NativeFunc = NativeCode;
  Result.CodeSize = CodeSize;
  Result.IRGraph = Ctx; // Preserve for potential tier-up

  return Result;
}

Expect<void> IRJitEngine::invoke(void *NativeFunc,
                                  const AST::FunctionType &FuncType,
                                  Span<const ValVariant> Args,
                                  Span<ValVariant> Rets,
                                  void **FuncTable, uint32_t FuncTableSize,
                                  void *GlobalBase,
                                  void *MemoryBase, uint32_t MemorySize) {
  if (!NativeFunc) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  (void)MemorySize; // Currently unused, but available for bounds checking

  const auto &ParamTypes = FuncType.getParamTypes();
  const auto &RetTypes = FuncType.getReturnTypes();
  
  // The IR-generated function has signature:
  //   return_type func(void** func_table, uint32_t table_size, 
  //                    void* global_base, void* mem_base, param1, param2, ...)
  // where param types match the wasm function signature.
  //
  // For POC, we handle common cases (0-4 params) with explicit casts.
  // A production implementation would use libffi or similar.
  
  uint64_t RetVal = 0;
  
  // Call based on number of parameters
  switch (ParamTypes.size()) {
  case 0: {
    // func(void** ft, uint32_t ts, void* gb, void* mb) -> ret
    if (RetTypes.empty()) {
      using FnType = void (*)(void**, uint32_t, void*, void*);
      reinterpret_cast<FnType>(NativeFunc)(FuncTable, FuncTableSize, GlobalBase, MemoryBase);
    } else {
      using FnType = uint64_t (*)(void**, uint32_t, void*, void*);
      RetVal = reinterpret_cast<FnType>(NativeFunc)(FuncTable, FuncTableSize, GlobalBase, MemoryBase);
    }
    break;
  }
  case 1: {
    // func(void** ft, uint32_t ts, void* gb, void* mb, arg0) -> ret
    uint64_t a0 = valVariantToRaw(Args[0]);
    if (RetTypes.empty()) {
      using FnType = void (*)(void**, uint32_t, void*, void*, uint64_t);
      reinterpret_cast<FnType>(NativeFunc)(FuncTable, FuncTableSize, GlobalBase, MemoryBase, a0);
    } else {
      using FnType = uint64_t (*)(void**, uint32_t, void*, void*, uint64_t);
      RetVal = reinterpret_cast<FnType>(NativeFunc)(FuncTable, FuncTableSize, GlobalBase, MemoryBase, a0);
    }
    break;
  }
  case 2: {
    // func(void** ft, uint32_t ts, void* gb, void* mb, arg0, arg1) -> ret
    uint64_t a0 = valVariantToRaw(Args[0]);
    uint64_t a1 = valVariantToRaw(Args[1]);
    if (RetTypes.empty()) {
      using FnType = void (*)(void**, uint32_t, void*, void*, uint64_t, uint64_t);
      reinterpret_cast<FnType>(NativeFunc)(FuncTable, FuncTableSize, GlobalBase, MemoryBase, a0, a1);
    } else {
      using FnType = uint64_t (*)(void**, uint32_t, void*, void*, uint64_t, uint64_t);
      RetVal = reinterpret_cast<FnType>(NativeFunc)(FuncTable, FuncTableSize, GlobalBase, MemoryBase, a0, a1);
    }
    break;
  }
  case 3: {
    // func(void** ft, uint32_t ts, void* gb, void* mb, arg0, arg1, arg2) -> ret
    uint64_t a0 = valVariantToRaw(Args[0]);
    uint64_t a1 = valVariantToRaw(Args[1]);
    uint64_t a2 = valVariantToRaw(Args[2]);
    if (RetTypes.empty()) {
      using FnType = void (*)(void**, uint32_t, void*, void*, uint64_t, uint64_t, uint64_t);
      reinterpret_cast<FnType>(NativeFunc)(FuncTable, FuncTableSize, GlobalBase, MemoryBase, a0, a1, a2);
    } else {
      using FnType = uint64_t (*)(void**, uint32_t, void*, void*, uint64_t, uint64_t, uint64_t);
      RetVal = reinterpret_cast<FnType>(NativeFunc)(FuncTable, FuncTableSize, GlobalBase, MemoryBase, a0, a1, a2);
    }
    break;
  }
  case 4: {
    // func(void** ft, uint32_t ts, void* gb, void* mb, arg0, arg1, arg2, arg3) -> ret
    uint64_t a0 = valVariantToRaw(Args[0]);
    uint64_t a1 = valVariantToRaw(Args[1]);
    uint64_t a2 = valVariantToRaw(Args[2]);
    uint64_t a3 = valVariantToRaw(Args[3]);
    if (RetTypes.empty()) {
      using FnType = void (*)(void**, uint32_t, void*, void*, uint64_t, uint64_t, uint64_t, uint64_t);
      reinterpret_cast<FnType>(NativeFunc)(FuncTable, FuncTableSize, GlobalBase, MemoryBase, a0, a1, a2, a3);
    } else {
      using FnType = uint64_t (*)(void**, uint32_t, void*, void*, uint64_t, uint64_t, uint64_t, uint64_t);
      RetVal = reinterpret_cast<FnType>(NativeFunc)(FuncTable, FuncTableSize, GlobalBase, MemoryBase, a0, a1, a2, a3);
    }
    break;
  }
  default:
    // More than 4 params not supported in POC
    // Production would need libffi or varargs-based approach
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // Process return value
  if (!RetTypes.empty() && !Rets.empty()) {
    Rets[0] = rawToValVariant(RetVal, RetTypes[0]);
  }

  return {};
}

void IRJitEngine::release(void *NativeFunc, size_t) noexcept {
  if (!NativeFunc) {
    return;
  }

  // Find and remove from tracked buffers
  for (auto it = CodeBuffers.begin(); it != CodeBuffers.end(); ++it) {
    if (it->Code == NativeFunc) {
      freeExecutable(it->Code, it->Size);
      CodeBuffers.erase(it);
      return;
    }
  }
}

void IRJitEngine::releaseIRGraph(ir_ctx *Ctx) noexcept {
  if (Ctx) {
    ir_free(Ctx);
  }
}

void *IRJitEngine::allocateExecutable(size_t Size) {
  // Allocate memory with read-write permissions initially (W^X compliant)
  void *Mem = mmap(nullptr, Size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (Mem == MAP_FAILED) {
    return nullptr;
  }

  return Mem;
}

void IRJitEngine::freeExecutable(void *Ptr, size_t Size) noexcept {
  if (Ptr && Size > 0) {
    munmap(Ptr, Size);
  }
}

uint64_t IRJitEngine::valVariantToRaw(const ValVariant &Val) const noexcept {
  // Convert ValVariant to raw 64-bit value
  // This is simplified - actual implementation needs proper type handling
  return Val.get<uint64_t>();
}

ValVariant IRJitEngine::rawToValVariant(uint64_t Raw,
                                         ValType Type) const noexcept {
  // Convert raw value back to ValVariant based on type
  auto Code = Type.getCode();
  if (Code == TypeCode::I32) {
    return ValVariant(static_cast<uint32_t>(Raw));
  } else if (Code == TypeCode::I64) {
    return ValVariant(static_cast<uint64_t>(Raw));
  } else if (Code == TypeCode::F32) {
    float F;
    std::memcpy(&F, &Raw, sizeof(float));
    return ValVariant(F);
  } else if (Code == TypeCode::F64) {
    double D;
    std::memcpy(&D, &Raw, sizeof(double));
    return ValVariant(D);
  } else {
    return ValVariant(static_cast<uint64_t>(0));
  }
}

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT

