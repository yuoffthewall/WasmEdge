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

#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

namespace {
static thread_local jmp_buf g_termination_buf;
} // namespace

// Thread-local JitExecEnv pointer, published by IRJitEngine::invoke before
// dispatching into tier-1 JIT code. Tier-2 t1_thunks read this to get the
// JitExecEnv* needed for the tier-1 ABI (arg0). Exposed with external C
// linkage so tier2_compiler.cpp can compute its TLS offset at JIT compile
// time and emit a direct %fs:OFFSET load instead of a function call.
extern "C" {
thread_local WasmEdge::VM::JitExecEnv *wasmedge_tier2_jit_env_tls;
}

// Legacy accessor — the hot path now uses a direct %fs:offset inline asm
// emitted by tier2_compiler.cpp. Kept for the ORC absolute-symbol fallback
// and any out-of-tree consumers.
extern "C" void *wasmedge_tier2_get_jit_env(void) {
  return wasmedge_tier2_jit_env_tls;
}

// Returns the byte offset of wasmedge_tier2_jit_env_tls from the thread
// pointer (%fs:0 on x86_64-linux). The offset is fixed once the DSO is
// loaded and is the same for all threads, so the JIT can hardcode it.
extern "C" ptrdiff_t wasmedge_tier2_get_jit_env_tls_offset(void) {
  uintptr_t TP;
  asm volatile("mov %%fs:0, %0" : "=r"(TP));
  return reinterpret_cast<uintptr_t>(&wasmedge_tier2_jit_env_tls) - TP;
}

namespace {

/// SIGSEGV guard for ir_jit_compile: the IR library can segfault on certain
/// complex IR patterns (e.g. nested loops with many PHIs). We install a
/// temporary signal handler to catch the crash and longjmp back to the caller,
/// allowing graceful fallback to the interpreter for that function.
/// Not using this for now to expose the failure.
/*
static thread_local sigjmp_buf g_compile_guard_buf;
static thread_local volatile bool g_compile_guard_active = false;

static void compileGuardHandler(int sig) {
  if (g_compile_guard_active) {
    g_compile_guard_active = false;
    siglongjmp(g_compile_guard_buf, 1);
  }
  // Not our guard — re-raise with default handler.
  signal(sig, SIG_DFL);
  raise(sig);
}

/// Compile with SIGSEGV/SIGABRT protection. Returns native code or nullptr on
/// crash.  SIGABRT is caught because the IR optimizer (SCCP, GCM) uses assert()
/// which calls abort() on invalid IR patterns the optimizer cannot handle.
static void *safeIrJitCompile(ir_ctx *ctx, int opt_level, size_t *size) {
  struct sigaction sa_new{}, sa_old_segv{}, sa_old_abrt{};
  sa_new.sa_handler = compileGuardHandler;
  sigemptyset(&sa_new.sa_mask);
  sa_new.sa_flags = 0; // No SA_RESTART — we want longjmp

  bool have_segv = sigaction(SIGSEGV, &sa_new, &sa_old_segv) == 0;
  bool have_abrt = sigaction(SIGABRT, &sa_new, &sa_old_abrt) == 0;

  if (!have_segv && !have_abrt) {
    // Can't install any handler — compile without guard.
    return ir_jit_compile(ctx, opt_level, size);
  }

  void *result = nullptr;
  g_compile_guard_active = true;
  if (sigsetjmp(g_compile_guard_buf, 1) == 0) {
    result = ir_jit_compile(ctx, opt_level, size);
  } else {
    // Caught SIGSEGV or SIGABRT during compilation.
    result = nullptr;
  }
  g_compile_guard_active = false;

  // Restore previous handlers.
  if (have_segv) sigaction(SIGSEGV, &sa_old_segv, nullptr);
  if (have_abrt) sigaction(SIGABRT, &sa_old_abrt, nullptr);
  return result;
}
*/

} // anonymous namespace

extern "C" void *wasmedge_ir_jit_get_termination_buf(void) {
  return &g_termination_buf;
}

/// OOB trap handler: longjmps back to invoke() with value 2 (MemoryOutOfBounds).
extern "C" void jit_oob_trap(void) {
  void *buf = wasmedge_ir_jit_get_termination_buf();
  if (buf) {
    longjmp(*static_cast<jmp_buf *>(buf), 2);
  }
  std::abort();
}

/// Trap stub for uncompiled wasm functions (e.g. functions starting with
/// `unreachable`).  Has the same signature as JIT-compiled functions so it
/// can be placed in FuncTable.  Longjmps with value 3 (→ Unreachable).
extern "C" int64_t jit_unreachable_trap(void * /*env*/, uint64_t * /*args*/) {
  void *buf = wasmedge_ir_jit_get_termination_buf();
  if (buf) {
    longjmp(*static_cast<jmp_buf *>(buf), 3);
  }
  std::abort();
}

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

  // Default O2; override with WASMEDGE_IR_JIT_OPT_LEVEL=0|1 for debug.
  int opt_level = 2;
  if (const char *e = std::getenv("WASMEDGE_IR_JIT_OPT_LEVEL")) {
    if (e[0] == '0' && e[1] == '\0')
      opt_level = 0;
    else if (e[0] == '1' && e[1] == '\0')
      opt_level = 1;
    else if (e[0] == '2' && e[1] == '\0')
      opt_level = 2;
  }

  static int _dbg_func_id = 0;
  int _dbg_cur_id = _dbg_func_id++;

  bool _dbg_dump = std::getenv("WASMEDGE_IR_JIT_DUMP") != nullptr;
  if (_dbg_dump) {
    char fname[256];
    snprintf(fname, sizeof(fname), "/tmp/wasmedge_ir_%03d_before.ir", _dbg_cur_id);
    FILE *f = fopen(fname, "w");
    if (f) { ir_save(Ctx, 0, f); fclose(f); }
  }

  size_t CodeSize = 0;
  void *NativeCode = ir_jit_compile(Ctx, opt_level, &CodeSize);

  if (!NativeCode) {
    spdlog::info("IR JIT: ir_jit_compile failed (func {})", _dbg_cur_id);
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  if (_dbg_dump) {
    char fname[256];
    snprintf(fname, sizeof(fname), "/tmp/wasmedge_ir_%03d_after.ir", _dbg_cur_id);
    FILE *f = fopen(fname, "w");
    if (f) { ir_save(Ctx, IR_SAVE_CFG, f); fclose(f); }
    // Dump machine code bytes for objdump
    snprintf(fname, sizeof(fname), "/tmp/wasmedge_ir_%03d.bin", _dbg_cur_id);
    FILE *bf = fopen(fname, "wb");
    if (bf) {
      fwrite(NativeCode, 1, CodeSize, bf);
      fclose(bf);
    }
  }

  // Track the code buffer
  CodeBuffers.push_back({NativeCode, CodeSize, CodeSize});

  // Register with GDB JIT interface so breakpoints / disassembly work
  {
    char gdb_name[64];
    snprintf(gdb_name, sizeof(gdb_name), "wasm_jit_%03d", _dbg_cur_id);
    ir_gdb_register(gdb_name, NativeCode, CodeSize, 0, 0);
  }

  CompileResult Result;
  Result.NativeFunc = NativeCode;
  Result.CodeSize = CodeSize;

  return Result;
}

Expect<void> IRJitEngine::invoke(void *NativeFunc,
                                  const AST::FunctionType &FuncType,
                                  Span<const ValVariant> Args,
                                  Span<ValVariant> Rets,
                                  void **FuncTable, uint32_t FuncTableSize,
                                  void *GlobalBase,
                                  void *MemoryBase, uint64_t MemorySize,
                                  DispatchEntry *Table0Dispatch,
                                  uint32_t Table0DispatchSize,
                                  uint32_t *CallCounters,
                                  uint32_t *BackEdgeCounters,
                                  void **OsrEntryTable) {
  if (!NativeFunc) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  const auto &ParamTypes = FuncType.getParamTypes();
  const auto &RetTypes = FuncType.getReturnTypes();

  JitExecEnv Env;
  Env.FuncTable = FuncTable;
  Env.FuncTableSize = FuncTableSize;
  Env._pad = 0;
  Env.GlobalBase = GlobalBase;
  Env.MemoryBase = MemoryBase;
  Env.HostCallFn = reinterpret_cast<void *>(&jit_host_call);
  Env.DirectOrHostFn = reinterpret_cast<void *>(&jit_direct_or_host);
  Env.MemoryGrowFn = reinterpret_cast<void *>(&jit_memory_grow);
  Env.MemorySizeFn = reinterpret_cast<void *>(&jit_memory_size);
  Env.CallIndirectFn = reinterpret_cast<void *>(&jit_call_indirect);
  Env.MemorySizeBytes = static_cast<uint64_t>(MemorySize);
  Env.Table0Dispatch = Table0Dispatch;
  Env.Table0DispatchSize = Table0DispatchSize;
  Env._pad2 = 0;
  Env.CallCounters = CallCounters;
  Env.TierUpNotifyFn = reinterpret_cast<void *>(&jit_tier_up_notify);
  Env.BackEdgeCounters = BackEdgeCounters;
  Env.OsrNotifyFn = reinterpret_cast<void *>(&jit_osr_notify);

  if (OsrLocalsFrame_.size() < OSR_LOCALS_FRAME_SLOTS)
    OsrLocalsFrame_.assign(OSR_LOCALS_FRAME_SLOTS, 0);
  Env.OsrLocalsFrame = OsrLocalsFrame_.data();
  Env.OsrLocalsFrameSize = OSR_LOCALS_FRAME_SLOTS;
  Env._pad3 = 0;
  Env.OsrEntryTable = OsrEntryTable;

  ArgsBuffer_.resize(ParamTypes.size());
  for (size_t i = 0; i < ParamTypes.size(); ++i)
    ArgsBuffer_[i] = valVariantToRaw(Args[i], ParamTypes[i]);
  uint64_t *ArgsData = ArgsBuffer_.empty() ? nullptr : ArgsBuffer_.data();

  // Publish the current env to the per-thread slot so tier-2 t1_thunks can
  // dispatch tier-2 → tier-1 calls via FuncTable[idx], which require a
  // JitExecEnv* as arg0 that is not otherwise reachable from tier-2 code.
  // Save/restore for re-entrancy (e.g. a host callback re-enters invoke).
  WasmEdge::VM::JitExecEnv *SavedTlsEnv = wasmedge_tier2_jit_env_tls;
  wasmedge_tier2_jit_env_tls = &Env;
  struct TlsRestore {
    WasmEdge::VM::JitExecEnv *Prev;
    ~TlsRestore() { wasmedge_tier2_jit_env_tls = Prev; }
  } TlsRestore_{SavedTlsEnv};

  void *termBuf = wasmedge_ir_jit_get_termination_buf();
  if (termBuf) {
    int jmpVal = setjmp(*static_cast<jmp_buf *>(termBuf));
    if (jmpVal == 2) {
      // OOB trap from jit_oob_trap (inline bounds check)
      return Unexpect(ErrCode::Value::MemoryOutOfBounds);
    }
    if (jmpVal == 3) {
      // Unreachable trap from jit_unreachable_trap (uncompiled trap stub)
      return Unexpect(ErrCode::Value::Unreachable);
    }
    if (jmpVal != 0) {
      // Termination (e.g. proc_exit via jit_host_call)
      return Unexpect(ErrCode::Value::Terminated);
    }
  }

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

uint64_t IRJitEngine::valVariantToRaw(const ValVariant &Val,
                                      ValType Type) const noexcept {
  auto Code = Type.getCode();
  if (Code == TypeCode::I32) {
    return static_cast<uint64_t>(Val.get<uint32_t>());
  }
  if (Code == TypeCode::I64) {
    return Val.get<uint64_t>();
  }
  if (Code == TypeCode::F32) {
    uint64_t Raw = 0;
    float F = Val.get<float>();
    std::memcpy(&Raw, &F, sizeof(float));
    return Raw;
  }
  if (Code == TypeCode::F64) {
    uint64_t Raw = 0;
    double D = Val.get<double>();
    std::memcpy(&Raw, &D, sizeof(double));
    return Raw;
  }
  return 0;
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

