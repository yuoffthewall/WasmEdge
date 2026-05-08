// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC
//
//===-- tools/sightglass_engine/engine.cpp - sightglass engine plugin -----===//
//
// Implements the 5 C symbols sightglass-cli's recorder dlopens:
//   wasm_bench_create / free / compile / instantiate / execute
// (matching crates/recorder/src/bench_api.rs).
//
// Mode (Interpreter / IR_JIT / LLVM JIT) is selected by env var
// `WASMEDGE_SIGHTGLASS_MODE`. Other engine knobs (TIER2, OSR, OPT_LEVEL, …)
// are read directly by the IR JIT pipeline from `getenv` — the shim doesn't
// need to plumb them.
//
// The bench host module's `start` / `end` calls forward to sightglass's
// execution-phase timer callbacks, narrowing the execution measurement to
// the bench-marked region inside the wasm. Compilation and instantiation
// phases are timed by the shim around `loadWasm`+`validate` and
// `instantiate` respectively.
//
//===----------------------------------------------------------------------===//

#include "wasm_bench_config.h"

#include "common/configure.h"
#include "common/errcode.h"
#include "common/types.h"
#include "host/wasi/wasimodule.h"
#include "plugin/plugin.h"
#include "runtime/callingframe.h"
#include "runtime/hostfunc.h"
#include "runtime/instance/module.h"
#include "vm/vm.h"

#include <spdlog/sinks/ansicolor_sink.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace std::literals;

namespace {

// ---- bench:start / bench:end host module ----------------------------------

class BenchModule final : public WasmEdge::Runtime::Instance::ModuleInstance {
public:
  BenchModule(std::uint8_t *Timer, void (*StartFn)(std::uint8_t *),
              void (*EndFn)(std::uint8_t *))
      : ModuleInstance("bench"), Timer(Timer), StartFn(StartFn), EndFn(EndFn) {
    addHostFunc("start", std::make_unique<BenchStart>(*this));
    addHostFunc("end", std::make_unique<BenchEnd>(*this));
  }

  std::uint8_t *Timer;
  void (*StartFn)(std::uint8_t *);
  void (*EndFn)(std::uint8_t *);

private:
  class BenchStart : public WasmEdge::Runtime::HostFunction<BenchStart> {
  public:
    explicit BenchStart(BenchModule &M) : Mod(M) {}
    WasmEdge::Expect<void>
    body(const WasmEdge::Runtime::CallingFrame &) {
      Mod.StartFn(Mod.Timer);
      return {};
    }
    BenchModule &Mod;
  };
  class BenchEnd : public WasmEdge::Runtime::HostFunction<BenchEnd> {
  public:
    explicit BenchEnd(BenchModule &M) : Mod(M) {}
    WasmEdge::Expect<void>
    body(const WasmEdge::Runtime::CallingFrame &) {
      Mod.EndFn(Mod.Timer);
      return {};
    }
    BenchModule &Mod;
  };
};

// ---- per-benchmark engine state -------------------------------------------

struct EngineState {
  WasmBenchConfig Cfg{};
  WasmEdge::Configure Conf;
  // Declaration order is load-bearing. Members destruct in reverse
  // declaration order, and the VM holds references to the registered
  // bench module — so the VM must destruct *first* (joining its tier-2
  // worker thread, which may still be touching the bench imports). Keep
  // `Bench` declared *before* `VM_`.
  std::unique_ptr<BenchModule> Bench;
  std::unique_ptr<WasmEdge::VM::VM> VM_;
  std::filesystem::path WasmTmpPath; // wasm bytes spilled to disk; cleaned in free

  std::string WorkingDir;
  std::string StdoutPath;
  std::string StderrPath;
  std::string StdinPath; // empty => no stdin redirect

  int StdoutFd{-1};
  int StderrFd{-1};
  int StdinFd{-1};
};

WasmEdge::CompilerConfigure::OptimizationLevel parseOptLevel() {
  using OL = WasmEdge::CompilerConfigure::OptimizationLevel;
  if (const char *e = std::getenv("WASMEDGE_IR_JIT_OPT_LEVEL")) {
    switch (std::atoi(e)) {
    case 0:
      return OL::O0;
    case 1:
      return OL::O1;
    case 2:
      return OL::O2;
    case 3:
      return OL::O3;
    default:
      break;
    }
  }
  return OL::O2;
}

} // namespace

// ===========================================================================
// sightglass C ABI
// ===========================================================================
//
// Explicitly mark each exported function with `default` visibility and
// `used` so LTO doesn't drop them as "no internal callers" while building
// the shared library — sightglass-cli `dlopen`s and `dlsym`s these by
// name.

#define SIGHTGLASS_API \
  extern "C" __attribute__((visibility("default"), used))

SIGHTGLASS_API int wasm_bench_create(WasmBenchConfig Cfg, void **OutEngine) {
  // Redirect WasmEdge's spdlog from stdout (the default — see
  // lib/common/spdlog.cpp:18,21) to stderr. sightglass's recorder uses
  // the subprocess's stdout as the IPC channel for serialised
  // measurement records; any log line on stdout corrupts the parent's
  // parser ("invalid type: integer 2026, expected struct
  // MeasurementWire").
  static std::once_flag SpdlogRedirected;
  std::call_once(SpdlogRedirected, [] {
    auto Sink =
        std::make_shared<spdlog::sinks::ansicolor_stderr_sink_mt>();
    spdlog::set_default_logger(
        std::make_shared<spdlog::logger>("wasmedge_engine", Sink));
  });

  // Load installed plugins (WASI-NN, etc.) once. This honours WASMEDGE_PLUGIN_PATH
  // — sightglass-cli runs the engine in subprocesses, so any caller wanting
  // a plugin from a non-standard location should set that env var. We do
  // this even for benchmarks that don't import wasi_ephemeral_nn; the cost
  // is one dlopen per plugin per process and they're idempotent.
  static std::once_flag PluginsLoaded;
  std::call_once(PluginsLoaded, [] {
    WasmEdge::Plugin::Plugin::loadFromDefaultPaths();
  });

  auto Eng = std::make_unique<EngineState>();
  Eng->Cfg = Cfg;

  Eng->WorkingDir.assign(reinterpret_cast<const char *>(Cfg.working_dir_ptr),
                         Cfg.working_dir_len);
  Eng->StdoutPath.assign(reinterpret_cast<const char *>(Cfg.stdout_path_ptr),
                         Cfg.stdout_path_len);
  Eng->StderrPath.assign(reinterpret_cast<const char *>(Cfg.stderr_path_ptr),
                         Cfg.stderr_path_len);
  if (Cfg.stdin_path_ptr != nullptr && Cfg.stdin_path_len > 0) {
    Eng->StdinPath.assign(reinterpret_cast<const char *>(Cfg.stdin_path_ptr),
                          Cfg.stdin_path_len);
  }

  // Open stdout/stderr files. WASI takes ownership via initWithFds.
  Eng->StdoutFd =
      ::open(Eng->StdoutPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (Eng->StdoutFd < 0) {
    return 1;
  }
  Eng->StderrFd =
      ::open(Eng->StderrPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (Eng->StderrFd < 0) {
    ::close(Eng->StdoutFd);
    return 1;
  }
  if (!Eng->StdinPath.empty()) {
    Eng->StdinFd = ::open(Eng->StdinPath.c_str(), O_RDONLY);
    if (Eng->StdinFd < 0) {
      ::close(Eng->StdoutFd);
      ::close(Eng->StderrFd);
      return 1;
    }
  }

  // Mode: Interpreter / IR_JIT / JIT (LLVM). Default IR_JIT.
  const char *ModeEnv = std::getenv("WASMEDGE_SIGHTGLASS_MODE");
  std::string Mode = (ModeEnv != nullptr) ? ModeEnv : "IR_JIT";
  const bool IsInterp = (Mode == "Interpreter");
  const bool IsLLVMJIT = (Mode == "JIT");
  // IR_JIT is the default: !IsInterp && !IsLLVMJIT.

  Eng->Conf.getRuntimeConfigure().setForceInterpreter(IsInterp);
  Eng->Conf.getRuntimeConfigure().setEnableJIT(IsLLVMJIT);
  Eng->Conf.getCompilerConfigure().setOptimizationLevel(parseOptLevel());
  Eng->Conf.addHostRegistration(WasmEdge::HostRegistration::Wasi);

  *OutEngine = Eng.release();
  return 0;
}

SIGHTGLASS_API int wasm_bench_compile(const void *Engine,
                                      const std::uint8_t *Buf,
                                      std::size_t Len) {
  auto *Eng = const_cast<EngineState *>(static_cast<const EngineState *>(Engine));
  Eng->Cfg.compilation_start(Eng->Cfg.compilation_timer);

  // Build the VM here so loadWasm + IR-JIT lowering land in the
  // compilation phase. (IR JIT lowering currently runs at instantiate-time;
  // wasm_bench_instantiate captures that cost.)
  Eng->VM_ = std::make_unique<WasmEdge::VM::VM>(Eng->Conf);

  // Bench host module (must be registered before loadWasm).
  Eng->Bench = std::make_unique<BenchModule>(Eng->Cfg.execution_timer,
                                              Eng->Cfg.execution_start,
                                              Eng->Cfg.execution_end);
  if (auto R = Eng->VM_->registerModule(*Eng->Bench); !R) {
    Eng->Cfg.compilation_end(Eng->Cfg.compilation_timer);
    return 1;
  }

  // WASI-NN does NOT need explicit registerModule here: the VM constructor
  // already iterates Plugin::plugins() and auto-registers each plugin's
  // module instances (see lib/vm/vm.cpp:152 + unsafeRegisterPlugInHosts).
  // Manually registering would collide with the auto-created instance and
  // fail instantiate with "module name conflict, Code: 0x300". We just
  // need the plugin loaded before the VM is constructed, which we did
  // once-globally in wasm_bench_create.

  // Spill wasm to a tmpfile and load via path. The Span<const Byte>
  // overload of loadWasm has caused tier-2 worker crashes (LLVM IR
  // construction races / use-after-free of the bytes view), and the
  // existing in-tree harness uses loadWasm(path), so we mirror that.
  {
    char TmpName[] = "/tmp/sg_engine_wasm_XXXXXX.wasm";
    int Fd = ::mkstemps(TmpName, 5);
    if (Fd < 0) {
      Eng->Cfg.compilation_end(Eng->Cfg.compilation_timer);
      return 1;
    }
    std::size_t Off = 0;
    while (Off < Len) {
      ssize_t W = ::write(Fd, Buf + Off, Len - Off);
      if (W < 0) {
        ::close(Fd);
        Eng->Cfg.compilation_end(Eng->Cfg.compilation_timer);
        return 1;
      }
      Off += static_cast<std::size_t>(W);
    }
    ::close(Fd);
    Eng->WasmTmpPath = TmpName;
  }
  if (auto R = Eng->VM_->loadWasm(Eng->WasmTmpPath); !R) {
    Eng->Cfg.compilation_end(Eng->Cfg.compilation_timer);
    return 1;
  }
  if (auto R = Eng->VM_->validate(); !R) {
    Eng->Cfg.compilation_end(Eng->Cfg.compilation_timer);
    return 1;
  }

  Eng->Cfg.compilation_end(Eng->Cfg.compilation_timer);
  return 0;
}

SIGHTGLASS_API int wasm_bench_instantiate(const void *Engine) {
  auto *Eng = const_cast<EngineState *>(static_cast<const EngineState *>(Engine));

  // Configure WASI before timing instantiation. WASI init is setup, not
  // engine work — the IR-JIT lowering inside instantiate() is what we want
  // to attribute to this phase.
  auto *WasiMod = dynamic_cast<WasmEdge::Host::WasiModule *>(
      Eng->VM_->getImportModule(WasmEdge::HostRegistration::Wasi));
  if (WasiMod == nullptr) {
    return 1;
  }

  // Preopen working_dir at both `.` and `/` so kernels using either style
  // of relative path see the input files sightglass placed there.
  std::vector<std::string> Dirs;
  Dirs.emplace_back(".:" + Eng->WorkingDir);
  Dirs.emplace_back("/:" + Eng->WorkingDir);

  // WasmEdge's WASI initWithFds prepends ProgramName to Args, so we want
  // Args empty: argv ends up as just [ProgramName] (argc == 1). Some
  // kernels — gcc-loops being the canonical example — branch on
  // `argc > 1` to enable extra-verbose prints, which then produce
  // stdout that diverges from sightglass's golden expected file.
  const std::string ProgramName = "benchmark.wasm";
  std::vector<std::string> Args;
  std::vector<std::string> Envs;

  if (auto R = WasiMod->initWithFds(
          Dirs, ProgramName, Args, Envs,
          Eng->StdinFd >= 0 ? Eng->StdinFd : 0, // 0 = host stdin if no path
          Eng->StdoutFd, Eng->StderrFd);
      !R) {
    return 1;
  }

  Eng->Cfg.instantiation_start(Eng->Cfg.instantiation_timer);
  auto R = Eng->VM_->instantiate();
  Eng->Cfg.instantiation_end(Eng->Cfg.instantiation_timer);
  return R ? 0 : 1;
}

SIGHTGLASS_API int wasm_bench_execute(const void *Engine) {
  auto *Eng = const_cast<EngineState *>(static_cast<const EngineState *>(Engine));
  std::vector<WasmEdge::ValVariant> NoArgs;
  std::vector<WasmEdge::ValType> NoTypes;
  auto R = Eng->VM_->execute("_start", NoArgs, NoTypes);
  // proc_exit triggers ErrCode::Terminated, which WASI uses to signal
  // normal termination. Treat that as success.
  if (!R && R.error() != WasmEdge::ErrCode::Value::Terminated) {
    return 1;
  }
  return 0;
}

SIGHTGLASS_API void wasm_bench_free(const void *Engine) {
  auto *Eng = const_cast<EngineState *>(static_cast<const EngineState *>(Engine));
  std::error_code Ec;
  if (!Eng->WasmTmpPath.empty()) {
    std::filesystem::remove(Eng->WasmTmpPath, Ec);
  }
  // WASI took ownership of stdin/stdout/stderr fds via initWithFds; closing
  // them here would double-free. Leave them; WASI's destruction will close.
  delete Eng;
}
