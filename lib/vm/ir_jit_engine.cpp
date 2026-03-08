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

#include <cstdlib>
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

  // Use IR's built-in JIT compilation. Default O2; override with WASMEDGE_IR_JIT_OPT_LEVEL=0|1 for debug.
  int opt_level = 2;
  if (const char *e = std::getenv("WASMEDGE_IR_JIT_OPT_LEVEL")) {
    if (e[0] == '0' && e[1] == '\0') opt_level = 0;
    else if (e[0] == '1' && e[1] == '\0') opt_level = 1;
  }
  size_t CodeSize = 0;
  void *NativeCode = ir_jit_compile(Ctx, opt_level, &CodeSize);
  
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

  (void)MemorySize;

  const auto &ParamTypes = FuncType.getParamTypes();
  const auto &RetTypes = FuncType.getReturnTypes();

  JitExecEnv Env;
  Env.FuncTable = FuncTable;
  Env.FuncTableSize = FuncTableSize;
  Env._pad = 0;
  Env.GlobalBase = GlobalBase;
  Env.MemoryBase = MemoryBase;

  std::vector<uint64_t> ArgsRaw(ParamTypes.size());
  for (size_t i = 0; i < ParamTypes.size(); ++i)
    ArgsRaw[i] = valVariantToRaw(Args[i]);
  uint64_t *ArgsData = ArgsRaw.empty() ? nullptr : ArgsRaw.data();

  // Uniform JIT signature: ret func(JitExecEnv* env, uint64_t* args)
  if (RetTypes.empty()) {
    using Fn = void (*)(JitExecEnv *, uint64_t *);
    reinterpret_cast<Fn>(NativeFunc)(&Env, ArgsData);
  } else if (!Rets.empty()) {
    auto Code = RetTypes[0].getCode();
    if (Code == TypeCode::F32) {
      using Fn = float (*)(JitExecEnv *, uint64_t *);
      float F = reinterpret_cast<Fn>(NativeFunc)(&Env, ArgsData);
      Rets[0] = ValVariant(F);
    } else if (Code == TypeCode::F64) {
      using Fn = double (*)(JitExecEnv *, uint64_t *);
      double D = reinterpret_cast<Fn>(NativeFunc)(&Env, ArgsData);
      Rets[0] = ValVariant(D);
    } else {
      using Fn = uint64_t (*)(JitExecEnv *, uint64_t *);
      uint64_t Raw = reinterpret_cast<Fn>(NativeFunc)(&Env, ArgsData);
      Rets[0] = rawToValVariant(Raw, RetTypes[0]);
    }
  } else {
    using Fn = uint64_t (*)(JitExecEnv *, uint64_t *);
    reinterpret_cast<Fn>(NativeFunc)(&Env, ArgsData);
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

