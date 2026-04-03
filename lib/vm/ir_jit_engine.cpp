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
#include <unistd.h>

namespace {
static thread_local jmp_buf g_termination_buf;

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

  // Snapshot IR text and ret_type BEFORE ir_jit_compile (which mutates the context).
  // Tier-2 reloads this text into a fresh ir_ctx for LLVM emission.
  // Only serialize when tier-2 is enabled — ir_save is expensive for large functions.
  uint8_t RetType = Ctx->ret_type;
  std::string IRText;
  static const bool Tier2Enabled = [] {
    const char *E = std::getenv("WASMEDGE_TIER2_ENABLE");
    return E && E[0] == '1' && E[1] == '\0';
  }();
  if (Tier2Enabled) {
    char *buf = nullptr;
    size_t len = 0;
    FILE *memf = open_memstream(&buf, &len);
    if (memf) {
      ir_save(Ctx, 0, memf);
      fclose(memf);
      if (buf && len > 0) {
        IRText.assign(buf, len);
      }
      free(buf);
    }
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
  Result.IRText = std::move(IRText);
  Result.RetType = RetType;

  return Result;
}

Expect<void> IRJitEngine::invoke(void *NativeFunc,
                                  const AST::FunctionType &FuncType,
                                  Span<const ValVariant> Args,
                                  Span<ValVariant> Rets,
                                  void **FuncTable, uint32_t FuncTableSize,
                                  void *GlobalBase,
                                  void *MemoryBase, uint64_t MemorySize,
                                  uint32_t *CallCounters) {
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
  Env.CallCounters = CallCounters;
  Env.TierUpNotifyFn = reinterpret_cast<void *>(&jit_tier_up_notify);

  ArgsBuffer_.resize(ParamTypes.size());
  for (size_t i = 0; i < ParamTypes.size(); ++i)
    ArgsBuffer_[i] = valVariantToRaw(Args[i], ParamTypes[i]);
  uint64_t *ArgsData = ArgsBuffer_.empty() ? nullptr : ArgsBuffer_.data();

  void *termBuf = wasmedge_ir_jit_get_termination_buf();
  if (termBuf) {
    int jmpVal = setjmp(*static_cast<jmp_buf *>(termBuf));
    if (jmpVal == 2) {
      // OOB trap from jit_oob_trap (inline bounds check)
      return Unexpect(ErrCode::Value::MemoryOutOfBounds);
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

// ---------------------------------------------------------------------------
// PltStubTable — pre-allocated PLT stubs for direct JIT-to-JIT calls.
// ---------------------------------------------------------------------------

PltStubTable::~PltStubTable() noexcept {
  if (Base_ && Size_ > 0)
    munmap(Base_, Size_);
}

bool PltStubTable::allocate(uint32_t count) {
#if !defined(__x86_64__)
  // PLT stubs are x86-64 only for now.
  (void)count;
  return false;
#else
  if (count == 0)
    return true;
  Count_ = count;
  size_t raw = static_cast<size_t>(count) * StubSize;
  size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  Size_ = (raw + pageSize - 1) & ~(pageSize - 1);

  Base_ = mmap(nullptr, Size_, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Base_ == MAP_FAILED) {
    Base_ = nullptr;
    return false;
  }

  // Emit x86-64 stub for each function index i (16 bytes per stub):
  //   mov rax, [rdi]          ; 48 8b 07        (3B) load env->FuncTable (offset 0)
  //   mov rax, [rax + i*8]    ; 48 8b 80 <d32>  (7B) load FuncTable[i]
  //   jmp rax                 ; ff e0           (2B) tail-jump to callee
  //   nop (4-byte pad)        ; 0f 1f 40 00     (4B) align to 16
  // rdi = JitExecEnv* and rsi = uint64_t* args are preserved for the callee.
  uint8_t *p = static_cast<uint8_t *>(Base_);
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t *s = p + static_cast<size_t>(i) * StubSize;
    int32_t off = static_cast<int32_t>(static_cast<uint64_t>(i) * 8);
    // mov rax, [rdi]
    s[0] = 0x48; s[1] = 0x8b; s[2] = 0x07;
    // mov rax, [rax + off]
    s[3] = 0x48; s[4] = 0x8b; s[5] = 0x80;
    std::memcpy(&s[6], &off, 4);
    // jmp rax
    s[10] = 0xff; s[11] = 0xe0;
    // 4-byte NOP
    s[12] = 0x0f; s[13] = 0x1f; s[14] = 0x40; s[15] = 0x00;
  }

  // W^X: flip to read+execute.
  if (mprotect(Base_, Size_, PROT_READ | PROT_EXEC) != 0) {
    munmap(Base_, Size_);
    Base_ = nullptr;
    return false;
  }

  spdlog::debug("PLT stub table: {} stubs at {}", count, Base_);
  return true;
#endif
}

void *PltStubTable::getStub(uint32_t i) const noexcept {
  if (!Base_ || i >= Count_)
    return nullptr;
  return static_cast<uint8_t *>(Base_) + static_cast<size_t>(i) * StubSize;
}

PltStubTable *IRJitEngine::createStubTable(uint32_t funcCount) {
  auto tbl = std::make_unique<PltStubTable>();
  if (!tbl->allocate(funcCount))
    return nullptr;
  StubTables_.push_back(std::move(tbl));
  return StubTables_.back().get();
}

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT

