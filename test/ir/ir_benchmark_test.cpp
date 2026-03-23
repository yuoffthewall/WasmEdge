// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- test/ir/ir_benchmark_test.cpp - IR JIT Benchmark Tests ------------===//
//
// Benchmark tests for IR JIT execution using integer-only algorithms.
// Based on popular Sightglass benchmarks (fibonacci, ackermann, etc.)
// Includes Sightglass suite: WASI stubs, bench host module, Interpreter vs JIT.
//
//===----------------------------------------------------------------------===//

#include "common/configure.h"
#include "common/defines.h"
#include "common/spdlog.h"
#include "executor/executor.h"
#include "loader/loader.h"
#include "runtime/hostfunc.h"
#include "runtime/instance/module.h"
#include "validator/validator.h"
#include "vm/vm.h"
#ifdef WASMEDGE_USE_LLVM
#include "llvm/codegen.h"
#include "llvm/compiler.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <vector>

namespace {

// ============================================================================
// Sightglass: Minimal WASI types for stub (ABI-compatible with wasi_snapshot_preview1)
// ============================================================================
namespace SightglassWasi {
constexpr uint16_t WASI_ERRNO_SUCCESS = 0;
constexpr uint16_t WASI_ERRNO_BADF = 8;     // EBADF: bad file descriptor
constexpr uint16_t WASI_ERRNO_NOENT = 44;  // ENOENT: no such file or directory
constexpr uint16_t WASI_ERRNO_NOTCAP = 58; // __WASI_ENOTCAP: capability unavailable
// __wasi_fdstat_t: 24 bytes (filetype u8, fdflags u16, rights_base u64, rights_inheriting u64)
struct FdStat {
  uint8_t fs_filetype;
  uint8_t _pad1;
  uint16_t fs_flags;
  uint32_t _pad2;
  uint64_t fs_rights_base;
  uint64_t fs_rights_inheriting;
};
static_assert(sizeof(FdStat) == 24, "fdstat size");
// __wasi_prestat_t: tag u8, pad 3, pr_name_len u32
struct Prestat {
  uint8_t tag;  // __WASI_PREOPENTYPE_DIR = 0
  uint8_t _pad[3];
  uint32_t pr_name_len;
};
static_assert(sizeof(Prestat) == 8, "prestat size");
} // namespace SightglassWasi

// ============================================================================
// Sightglass: WASI stub host module (no-op / dummy so kernels can instantiate)
// Optional stdout/stderr and exit code capture for correctness checks (Interpreter vs JIT).
// ============================================================================
struct StdioCapture {
  std::string stdout_;
  std::string stderr_;
  int32_t exitCode{-1};  // proc_exit(code); -1 = never called
};

// Virtual file descriptor state for serving preloaded file content via WASI stubs.
struct VirtualFd {
  std::string content;
  size_t offset = 0;
};

class WasiStubModule : public WasmEdge::Runtime::Instance::ModuleInstance {
public:
  explicit WasiStubModule(StdioCapture *Capture = nullptr) : ModuleInstance("wasi_snapshot_preview1"), Capture(Capture) {
    addHostFunc("fd_fdstat_get", std::make_unique<StubFdFdstatGet>());
    addHostFunc("fd_prestat_get", std::make_unique<StubFdPrestatGet>());
    addHostFunc("fd_prestat_dir_name", std::make_unique<StubFdPrestatDirName>());
    addHostFunc("environ_sizes_get", std::make_unique<StubEnvironSizesGet>());
    addHostFunc("environ_get", std::make_unique<StubEnvironGet>());
    addHostFunc("args_sizes_get", std::make_unique<StubArgsSizesGet>());
    addHostFunc("args_get", std::make_unique<StubArgsGet>());
    addHostFunc("fd_filestat_get", std::make_unique<StubFdFilestatGet>());
    addHostFunc("fd_close", std::make_unique<StubFdClose>(this));
    addHostFunc("fd_read", std::make_unique<StubFdRead>(this));
    addHostFunc("fd_seek", std::make_unique<StubFdSeek>());
    addHostFunc("fd_write", std::make_unique<StubFdWrite>(Capture));
    addHostFunc("path_open", std::make_unique<StubPathOpen>(this));
    addHostFunc("path_filestat_get", std::make_unique<StubPathFilestatGet>(this));
    addHostFunc("clock_time_get", std::make_unique<StubClockTimeGet>());
    addHostFunc("clock_res_get", std::make_unique<StubClockResGet>());
    addHostFunc("random_get", std::make_unique<StubRandomGet>());
    addHostFunc("fd_fdstat_set_flags", std::make_unique<StubFdFdstatSetFlags>());
    addHostFunc("path_remove_directory", std::make_unique<StubPathRemoveDirectory>());
    addHostFunc("path_unlink_file", std::make_unique<StubPathUnlinkFile>());
    addHostFunc("proc_exit", std::make_unique<StubProcExit>(Capture));
  }

  // Preload a virtual file that can be opened by the guest via path_open.
  // The path should match what the guest requests (e.g. "./shootout-ackermann.m.input"
  // or just "shootout-ackermann.m.input" — we try both with and without "./").
  void addVirtualFile(const std::string &path, std::string content) {
    virtualFiles[path] = std::move(content);
  }

  // Virtual file storage: filename -> content
  std::map<std::string, std::string> virtualFiles;
  // Open file descriptors: fd -> VirtualFd
  std::map<int32_t, VirtualFd> openFds;
  int32_t nextFd = 10;  // Start above stdin(0)/stdout(1)/stderr(2)

private:
  StdioCapture *Capture;
  using Expect = WasmEdge::Expect<uint32_t>;
  static WasmEdge::Runtime::Instance::MemoryInstance *
  getMem(const WasmEdge::Runtime::CallingFrame &Frame, uint32_t Index = 0) {
    return Frame.getMemoryByIndex(Index);
  }

  class StubFdFdstatGet : public WasmEdge::Runtime::HostFunction<StubFdFdstatGet> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, int32_t /* Fd */,
               uint32_t FdStatPtr) {
      auto *Mem = getMem(Frame);
      if (!Mem) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      SightglassWasi::FdStat st = {};
      st.fs_filetype = 3;  // directory
      st.fs_rights_base = 0xFFFFFFFFFFFFFFFFULL;
      st.fs_rights_inheriting = 0xFFFFFFFFFFFFFFFFULL;
      auto *ptr = Mem->getPointer<uint8_t *>(FdStatPtr);
      if (!ptr) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      std::memcpy(ptr, &st, sizeof(st));
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubFdPrestatGet : public WasmEdge::Runtime::HostFunction<StubFdPrestatGet> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, int32_t Fd,
               uint32_t PrestatPtr) {
      // Only fd 3 is pre-opened as "." (current directory).
      // Return BADF for any other fd to stop wasi-libc enumeration.
      if (Fd != 3) return SightglassWasi::WASI_ERRNO_BADF;
      auto *Mem = getMem(Frame);
      if (!Mem) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      SightglassWasi::Prestat p = {};
      p.tag = 0;  // dir
      p.pr_name_len = 1;  // "." is 1 byte
      auto *ptr = Mem->getPointer<uint8_t *>(PrestatPtr);
      if (!ptr) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      std::memcpy(ptr, &p, sizeof(p));
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubFdPrestatDirName : public WasmEdge::Runtime::HostFunction<StubFdPrestatDirName> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, int32_t /* Fd */,
               uint32_t PathBuf, uint32_t PathLen) {
      auto *Mem = getMem(Frame);
      if (!Mem) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      // Write "." as the pre-opened directory name
      if (PathLen >= 1) {
        auto *ptr = Mem->getPointer<char *>(PathBuf);
        if (ptr) ptr[0] = '.';
      }
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubEnvironSizesGet : public WasmEdge::Runtime::HostFunction<StubEnvironSizesGet> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, uint32_t EnvCntPtr,
               uint32_t EnvBufSizePtr) {
      auto *Mem = getMem(Frame);
      if (!Mem) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      const uint32_t zero = 0;
      auto *p1 = Mem->getPointer<uint32_t *>(EnvCntPtr);
      auto *p2 = Mem->getPointer<uint32_t *>(EnvBufSizePtr);
      if (!p1 || !p2) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      *p1 = zero;
      *p2 = zero;
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubEnvironGet : public WasmEdge::Runtime::HostFunction<StubEnvironGet> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &, uint32_t, uint32_t) {
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubArgsSizesGet : public WasmEdge::Runtime::HostFunction<StubArgsSizesGet> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, uint32_t ArgcPtr,
               uint32_t ArgvBufSizePtr) {
      auto *Mem = getMem(Frame);
      if (!Mem) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      const uint32_t zero = 0;
      auto *p1 = Mem->getPointer<uint32_t *>(ArgcPtr);
      auto *p2 = Mem->getPointer<uint32_t *>(ArgvBufSizePtr);
      if (!p1 || !p2) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      *p1 = zero;
      *p2 = zero;
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubArgsGet : public WasmEdge::Runtime::HostFunction<StubArgsGet> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &, uint32_t, uint32_t) {
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubFdFilestatGet : public WasmEdge::Runtime::HostFunction<StubFdFilestatGet> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &, int32_t, uint32_t) {
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubFdClose : public WasmEdge::Runtime::HostFunction<StubFdClose> {
  public:
    explicit StubFdClose(WasiStubModule *Mod) : Module(Mod) {}
    Expect body(const WasmEdge::Runtime::CallingFrame &, int32_t Fd) {
      if (Module) Module->openFds.erase(Fd);
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
    WasiStubModule *Module;
  };

  class StubFdRead : public WasmEdge::Runtime::HostFunction<StubFdRead> {
  public:
    explicit StubFdRead(WasiStubModule *Mod) : Module(Mod) {}
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, int32_t Fd,
                uint32_t IovsPtr, uint32_t IovsLen, uint32_t NreadPtr) {
      auto *Mem = getMem(Frame);
      if (!Mem) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);

      uint32_t totalRead = 0;

      // Check if this fd is a virtual file
      if (Module) {
        auto it = Module->openFds.find(Fd);
        if (it != Module->openFds.end()) {
          auto &vfd = it->second;
          // Read from virtual file content into iovecs
          for (uint32_t i = 0; i < IovsLen && vfd.offset < vfd.content.size(); i++) {
            // WASI iovec: { buf_ptr: u32, buf_len: u32 }
            auto *iovec = Mem->getPointer<uint32_t *>(IovsPtr + i * 8);
            if (!iovec) break;
            uint32_t bufPtr = iovec[0];
            uint32_t bufLen = iovec[1];
            size_t avail = vfd.content.size() - vfd.offset;
            size_t toRead = std::min(static_cast<size_t>(bufLen), avail);
            if (toRead > 0) {
              auto *dst = Mem->getPointer<uint8_t *>(bufPtr);
              if (dst) {
                std::memcpy(dst, vfd.content.data() + vfd.offset, toRead);
                vfd.offset += toRead;
                totalRead += static_cast<uint32_t>(toRead);
              }
            }
          }
          auto *p = Mem->getPointer<uint32_t *>(NreadPtr);
          if (p) *p = totalRead;
          return SightglassWasi::WASI_ERRNO_SUCCESS;
        }
      }

      // Default: return 0 bytes (EOF) for stdin and unknown fds
      auto *p = Mem->getPointer<uint32_t *>(NreadPtr);
      if (p) *p = 0;
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
    WasiStubModule *Module;
  };

  class StubFdSeek : public WasmEdge::Runtime::HostFunction<StubFdSeek> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, int32_t, int64_t, uint32_t /* whence */,
                uint32_t ResultPtr) {
      auto *Mem = getMem(Frame);
      if (Mem) {
        auto *p = Mem->getPointer<uint64_t *>(ResultPtr);
        if (p) *p = 0;
      }
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubFdWrite : public WasmEdge::Runtime::HostFunction<StubFdWrite> {
  public:
    explicit StubFdWrite(StdioCapture *Cap) : Capture(Cap) {}
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, int32_t Fd,
                uint32_t IovsPtr, uint32_t IovsLen, uint32_t NwrittenPtr) {
      auto *Mem = getMem(Frame);
      if (!Mem) return SightglassWasi::WASI_ERRNO_SUCCESS;
      uint32_t totalLen = 0;
      for (uint32_t i = 0; i < IovsLen; ++i) {
        auto *iov = Mem->getPointer<uint32_t *>(IovsPtr + i * 8);
        if (!iov) continue;
        uint32_t bufPtr = iov[0];
        uint32_t bufLen = iov[1];
        totalLen += bufLen;
        if (Capture && (Fd == 1 || Fd == 2) && bufLen > 0) {
          const uint8_t *src = Mem->getPointer<const uint8_t *>(bufPtr);
          if (src) {
            if (Fd == 1)
              Capture->stdout_.append(reinterpret_cast<const char *>(src), bufLen);
            else
              Capture->stderr_.append(reinterpret_cast<const char *>(src), bufLen);
          }
        }
      }
      auto *p = Mem->getPointer<uint32_t *>(NwrittenPtr);
      if (p) *p = totalLen;
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
    StdioCapture *Capture;
  };

  // path_filestat_get(dirfd, flags, path_ptr, path_len, filestat_ptr) -> errno
  // Fills __wasi_filestat_t so POSIX stat() (used by e.g. bz2 read_file) sees real size.
  class StubPathFilestatGet : public WasmEdge::Runtime::HostFunction<StubPathFilestatGet> {
  public:
    explicit StubPathFilestatGet(WasiStubModule *Mod) : Module(Mod) {}
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, int32_t /* DirFd */,
                uint32_t /* Flags */, uint32_t PathPtr, uint32_t PathLen,
                uint32_t FilestatPtr) {
      auto *Mem = getMem(Frame);
      if (!Mem) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      constexpr uint32_t kFilestatBytes = 64;
      auto *p = Mem->getPointer<uint8_t *>(FilestatPtr);
      if (!p) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      std::memset(p, 0, kFilestatBytes);
      uint64_t fileSize = 0;
      if (Module && PathLen > 0 && PathLen < 4096) {
        auto *pathBytes = Mem->getPointer<const char *>(PathPtr);
        if (pathBytes) {
          std::string reqPath(pathBytes, PathLen);
          std::string bareReq = reqPath;
          if (bareReq.size() >= 2 && bareReq.substr(0, 2) == "./")
            bareReq = bareReq.substr(2);
          for (const auto &[fname, content] : Module->virtualFiles) {
            std::string bareF = fname;
            if (bareF.size() >= 2 && bareF.substr(0, 2) == "./")
              bareF = bareF.substr(2);
            if (bareF == bareReq) {
              fileSize = static_cast<uint64_t>(content.size());
              break;
            }
          }
        }
      }
      // __wasi_filestat_t: filetype u8 at 16, size u64 at 32 (WASI preview1 layout).
      constexpr uint8_t WASI_FILETYPE_REGULAR_FILE = 4;
      p[16] = WASI_FILETYPE_REGULAR_FILE;
      std::memcpy(p + 32, &fileSize, sizeof(fileSize));
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
    WasiStubModule *Module;
  };

  class StubPathOpen : public WasmEdge::Runtime::HostFunction<StubPathOpen> {
  public:
    explicit StubPathOpen(WasiStubModule *Mod) : Module(Mod) {}
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, int32_t, uint32_t,
                uint32_t PathPtr, uint32_t PathLen, uint32_t, uint64_t, uint64_t, uint32_t,
                uint32_t OpenedFdPtr) {
      auto *Mem = getMem(Frame);
      if (!Mem) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);

      // Extract the path string from guest memory
      if (Module && PathLen > 0 && PathLen < 4096) {
        auto *pathBytes = Mem->getPointer<const char *>(PathPtr);
        if (pathBytes) {
          std::string reqPath(pathBytes, PathLen);
          // Try exact match, then with "./" prefix, then without "./" prefix
          std::string candidates[] = { reqPath };
          std::string bareReqPath = reqPath;
          if (bareReqPath.substr(0, 2) == "./")
            bareReqPath = bareReqPath.substr(2);

          for (const auto &[fname, content] : Module->virtualFiles) {
            std::string bareFname = fname;
            if (bareFname.substr(0, 2) == "./")
              bareFname = bareFname.substr(2);
            if (bareFname == bareReqPath) {
              int32_t fd = Module->nextFd++;
              Module->openFds[fd] = VirtualFd{content, 0};
              auto *p = Mem->getPointer<uint32_t *>(OpenedFdPtr);
              if (p) *p = static_cast<uint32_t>(fd);
              return SightglassWasi::WASI_ERRNO_SUCCESS;
            }
          }
          // File not found in virtual FS
          return SightglassWasi::WASI_ERRNO_NOENT;
        }
      }

      // Fallback: return fd 0 (legacy behavior)
      auto *p = Mem->getPointer<uint32_t *>(OpenedFdPtr);
      if (p) *p = 0;
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
    WasiStubModule *Module;
  };

  // WASI clock_time_get(clockid, precision_low, precision_high, time_ptr) -> errno
  // Writes a timestamp (u64) to time_ptr. Stub returns 0 and writes 0.
  class StubClockTimeGet : public WasmEdge::Runtime::HostFunction<StubClockTimeGet> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, uint32_t /* ClockId */,
                uint64_t /* Precision */, uint32_t TimePtr) {
      auto *Mem = getMem(Frame);
      if (!Mem) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      auto *p = Mem->getPointer<uint64_t *>(TimePtr);
      if (p) *p = 0;
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubRandomGet : public WasmEdge::Runtime::HostFunction<StubRandomGet> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, uint32_t BufPtr,
                uint32_t BufLen) {
      auto *Mem = getMem(Frame);
      if (Mem && BufLen > 0) {
        auto *p = Mem->getPointer<uint8_t *>(BufPtr);
        if (p) std::memset(p, 0, BufLen);
      }
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  // clock_res_get(clock_id, resolution_ptr) -> errno; writes u64 resolution (ns).
  class StubClockResGet : public WasmEdge::Runtime::HostFunction<StubClockResGet> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &Frame, uint32_t /* ClockId */,
                uint32_t ResPtr) {
      auto *Mem = getMem(Frame);
      if (!Mem) return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::HostFuncError);
      auto *p = Mem->getPointer<uint64_t *>(ResPtr);
      if (p) *p = 1; // 1 ns resolution
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubFdFdstatSetFlags : public WasmEdge::Runtime::HostFunction<StubFdFdstatSetFlags> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &, int32_t /* Fd */,
                uint32_t /* FdFlags */) {
      return SightglassWasi::WASI_ERRNO_SUCCESS;
    }
  };

  class StubPathRemoveDirectory : public WasmEdge::Runtime::HostFunction<StubPathRemoveDirectory> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &, int32_t /* Fd */,
                uint32_t /* PathPtr */, uint32_t /* PathLen */) {
      return SightglassWasi::WASI_ERRNO_NOTCAP;
    }
  };

  class StubPathUnlinkFile : public WasmEdge::Runtime::HostFunction<StubPathUnlinkFile> {
  public:
    Expect body(const WasmEdge::Runtime::CallingFrame &, int32_t /* Fd */,
                uint32_t /* PathPtr */, uint32_t /* PathLen */) {
      return SightglassWasi::WASI_ERRNO_NOTCAP;
    }
  };

  class StubProcExit : public WasmEdge::Runtime::HostFunction<StubProcExit> {
  public:
    explicit StubProcExit(StdioCapture *Cap) : Capture(Cap) {}
    // WASI proc_exit(code) never returns; the wasm then has unreachable.
    // Return Terminated so execution stops and we never run that unreachable.
    // Capture exit code for correctness comparison (Interpreter vs JIT).
    WasmEdge::Expect<void> body(const WasmEdge::Runtime::CallingFrame &, int32_t code) {
      if (Capture) Capture->exitCode = code;
      return WasmEdge::Unexpect(WasmEdge::ErrCode::Value::Terminated);
    }
    StdioCapture *Capture;
  };
};

// ============================================================================
// Sightglass: Bench host module (start/end timing for Work Time)
// ============================================================================
struct BenchEnv {
  std::chrono::high_resolution_clock::time_point startTime;
  std::chrono::nanoseconds workTimeNs{0};
};

class BenchModule : public WasmEdge::Runtime::Instance::ModuleInstance {
public:
  explicit BenchModule(BenchEnv &Env) : ModuleInstance("bench"), Env(Env) {
    addHostFunc("start", std::make_unique<BenchStart>(Env));
    addHostFunc("end", std::make_unique<BenchEnd>(Env));
    addHostFunc("stop", std::make_unique<BenchEnd>(Env)); // alias for Sightglass "end"
  }
  BenchEnv &getEnv() { return Env; }

private:
  BenchEnv &Env;

  class BenchStart : public WasmEdge::Runtime::HostFunction<BenchStart> {
  public:
    explicit BenchStart(BenchEnv &E) : Env(E) {}
    WasmEdge::Expect<void> body(const WasmEdge::Runtime::CallingFrame &) {
      Env.startTime = std::chrono::high_resolution_clock::now();
      return {};
    }
    BenchEnv &Env;
  };

  class BenchEnd : public WasmEdge::Runtime::HostFunction<BenchEnd> {
  public:
    explicit BenchEnd(BenchEnv &E) : Env(E) {}
    WasmEdge::Expect<void> body(const WasmEdge::Runtime::CallingFrame &) {
      auto endTime = std::chrono::high_resolution_clock::now();
      Env.workTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
          endTime - Env.startTime);
      return {};
    }
    BenchEnv &Env;
  };
};

class IRBenchmarkTest : public ::testing::Test {
protected:
  void SetUp() override {
    Conf.getCompilerConfigure().setOptimizationLevel(
        WasmEdge::CompilerConfigure::OptimizationLevel::O0);
  }

  std::filesystem::path getTestDataPath() {
    std::vector<std::filesystem::path> Candidates = {
        std::filesystem::path(__FILE__).parent_path() / "testdata",
        std::filesystem::current_path() / "test/ir/testdata",
        std::filesystem::current_path() / "../test/ir/testdata",
    };
    for (const auto &Path : Candidates) {
      if (std::filesystem::exists(Path))
        return Path;
    }
    return Candidates[0];
  }

  WasmEdge::Configure Conf;
};

// Helper to measure execution time
template <typename Func>
double measureTime(Func &&f, int iterations = 1) {
  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    f();
  }
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;
  return elapsed.count() / iterations;
}

// ============================================================================
// Correctness Tests
// ============================================================================

TEST_F(IRBenchmarkTest, FibonacciRecursive_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("fib_recursive");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Test known Fibonacci values
  struct TestCase {
    uint32_t n;
    uint32_t expected;
  };
  std::vector<TestCase> cases = {
      {0, 0},   {1, 1},   {2, 1},   {3, 2},   {4, 3},
      {5, 5},   {6, 8},   {7, 13},  {8, 21},  {9, 34},
      {10, 55}
  };

  for (const auto &tc : cases) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(tc.n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value()) << "Failed for n=" << tc.n;
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), tc.expected)
        << "fib_recursive(" << tc.n << ") should be " << tc.expected;
  }
}

TEST_F(IRBenchmarkTest, FibonacciIterative_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("fib_iterative");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Test known Fibonacci values
  struct TestCase {
    uint32_t n;
    uint32_t expected;
  };
  std::vector<TestCase> cases = {
      {0, 0},   {1, 1},   {2, 1},    {3, 2},     {4, 3},
      {5, 5},   {6, 8},   {7, 13},   {8, 21},    {9, 34},
      {10, 55}, {15, 610}, {20, 6765}, {25, 75025}
  };

  for (const auto &tc : cases) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(tc.n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value()) << "Failed for n=" << tc.n;
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), tc.expected)
        << "fib(" << tc.n << ") should be " << tc.expected;
  }
}

TEST_F(IRBenchmarkTest, Ackermann_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("ackermann");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Ackermann function values
  struct TestCase {
    uint32_t m, n, expected;
  };
  std::vector<TestCase> cases = {
      {0, 0, 1},   {0, 1, 2},   {0, 5, 6},
      {1, 0, 2},   {1, 1, 3},   {1, 5, 7},
      {2, 0, 3},   {2, 1, 5},   {2, 2, 7},
      {3, 0, 5},   {3, 1, 13},  {3, 2, 29}
  };

  for (const auto &tc : cases) {
    std::vector<WasmEdge::ValVariant> Args = {
        WasmEdge::ValVariant(tc.m), WasmEdge::ValVariant(tc.n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value()) << "Failed for ack(" << tc.m << ", " << tc.n << ")";
    ASSERT_EQ(ExecRes->size(), 1u);
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), tc.expected)
        << "ack(" << tc.m << ", " << tc.n << ") should be " << tc.expected;
  }
}

TEST_F(IRBenchmarkTest, SumToN_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("sum_to_n");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Test: sum(1..n) = n*(n+1)/2
  std::vector<uint32_t> testValues = {0, 1, 5, 10, 100, 1000, 10000};
  for (uint32_t n : testValues) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    uint32_t expected = n * (n + 1) / 2;
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), expected)
        << "sum_to_n(" << n << ") should be " << expected;
  }
}

TEST_F(IRBenchmarkTest, GCD_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("gcd");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  struct TestCase {
    uint32_t a, b, expected;
  };
  std::vector<TestCase> cases = {
      {48, 18, 6},   {100, 25, 25}, {17, 13, 1}, {270, 192, 6},
      {1071, 462, 21}, {0, 5, 5},     {5, 0, 5}
  };

  for (const auto &tc : cases) {
    std::vector<WasmEdge::ValVariant> Args = {
        WasmEdge::ValVariant(tc.a), WasmEdge::ValVariant(tc.b)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), tc.expected)
        << "gcd(" << tc.a << ", " << tc.b << ") should be " << tc.expected;
  }
}

TEST_F(IRBenchmarkTest, TestIsPrimeWrapper_Correctness) {
  // This tests JIT-to-JIT function calls by using a simple wrapper
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("test_is_prime");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Test known primes via wrapper
  std::vector<uint32_t> primes = {2, 3, 5, 7, 11};
  for (uint32_t p : primes) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(p)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 1u) 
        << "test_is_prime(" << p << ") should be 1 (prime)";
  }

  // Test non-primes via wrapper
  std::vector<uint32_t> nonPrimes = {0, 1, 4, 6, 8, 9, 10};
  for (uint32_t n : nonPrimes) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 0u)
        << "test_is_prime(" << n << ") should be 0 (not prime)";
  }
}

TEST_F(IRBenchmarkTest, IsPrime_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("is_prime");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  // Known primes
  std::vector<uint32_t> primes = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 97, 101};
  for (uint32_t p : primes) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(p)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 1u) << p << " should be prime";
  }

  // Non-primes
  std::vector<uint32_t> nonPrimes = {0, 1, 4, 6, 8, 9, 10, 15, 21, 100};
  for (uint32_t n : nonPrimes) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), 0u)
        << n << " should not be prime";
  }
}

TEST_F(IRBenchmarkTest, CountPrimes_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("count_primes");
  ASSERT_NE(FuncInst, nullptr);
  EXPECT_TRUE(FuncInst->isIRJitFunction()) << "Should be JIT compiled";

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  struct TestCase {
    uint32_t n, expected;
  };
  // π(n) - prime counting function
  std::vector<TestCase> cases = {
      {10, 4},   // 2, 3, 5, 7
      {20, 8},   // 2, 3, 5, 7, 11, 13, 17, 19
      {100, 25}, // Known: π(100) = 25
  };

  for (const auto &tc : cases) {
    std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(tc.n)};
    auto ExecRes = Executor.invoke(FuncInst, Args, ParamTypes);
    ASSERT_TRUE(ExecRes.has_value());
    EXPECT_EQ((*ExecRes)[0].first.get<uint32_t>(), tc.expected)
        << "count_primes(" << tc.n << ") should be " << tc.expected;
  }
}

// ============================================================================
// Performance Benchmarks
// ============================================================================

TEST_F(IRBenchmarkTest, Benchmark_FibonacciIterative) {
  // Disabled by default - run with --gtest_also_run_disabled_tests
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("fib_iterative");
  ASSERT_NE(FuncInst, nullptr);
  
  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  const int ITERATIONS = 100000;
  std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(uint32_t(35))};

  double avgTime = measureTime(
      [&]() { Executor.invoke(FuncInst, Args, ParamTypes); }, ITERATIONS);

  std::cout << "\n=== Fibonacci Iterative Benchmark ===" << std::endl;
  std::cout << "  fib(35) x " << ITERATIONS << " iterations" << std::endl;
  std::cout << "  JIT: " << FuncInst->isIRJitFunction() << std::endl;
  std::cout << "  Avg time per call: " << std::fixed << std::setprecision(3)
            << avgTime << " ms" << std::endl;
  std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
            << (1000.0 / avgTime) << " calls/sec" << std::endl;
}

TEST_F(IRBenchmarkTest, Benchmark_CountPrimes) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *FuncInst = ModInst->findFuncExports("count_primes");
  ASSERT_NE(FuncInst, nullptr);

  auto ParamTypes = FuncInst->getFuncType().getParamTypes();

  const int ITERATIONS = 1000;
  std::vector<WasmEdge::ValVariant> Args = {WasmEdge::ValVariant(uint32_t(1000))};

  double avgTime = measureTime(
      [&]() { Executor.invoke(FuncInst, Args, ParamTypes); }, ITERATIONS);

  std::cout << "\n=== Count Primes Benchmark ===" << std::endl;
  std::cout << "  count_primes(1000) x " << ITERATIONS << " iterations" << std::endl;
  std::cout << "  JIT: " << FuncInst->isIRJitFunction() << std::endl;
  std::cout << "  Avg time per call: " << std::fixed << std::setprecision(3)
            << avgTime << " ms" << std::endl;
  std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
            << (1000.0 / avgTime) << " calls/sec" << std::endl;
}

// ============================================================================
// Quicksort Tests - Memory operations + recursion
// ============================================================================

TEST_F(IRBenchmarkTest, Quicksort_Correctness) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  
  // Get function pointers
  auto *QuicksortFunc = ModInst->findFuncExports("quicksort");
  auto *IsSortedFunc = ModInst->findFuncExports("is_sorted");
  ASSERT_NE(QuicksortFunc, nullptr);
  ASSERT_NE(IsSortedFunc, nullptr);
  EXPECT_TRUE(QuicksortFunc->isIRJitFunction()) << "quicksort should be JIT compiled";
  EXPECT_TRUE(IsSortedFunc->isIRJitFunction()) << "is_sorted should be JIT compiled";

  // Get memory instance
  auto MemInsts = ModInst->getMemoryInstances();
  ASSERT_FALSE(MemInsts.empty());
  auto *MemInst = MemInsts[0];
  ASSERT_NE(MemInst, nullptr);

  auto QuicksortParams = QuicksortFunc->getFuncType().getParamTypes();
  auto IsSortedParams = IsSortedFunc->getFuncType().getParamTypes();

  // Test case 1: Sort a small array [5, 2, 8, 1, 9]
  {
    const uint32_t BASE = 0;
    const uint32_t LEN = 5;
    int32_t testData[] = {5, 2, 8, 1, 9};
    
    // Write test data to memory
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      ASSERT_NE(ptr, nullptr);
      *reinterpret_cast<int32_t *>(ptr) = testData[i];
    }

    // Call quicksort(base=0, low=0, high=4)
    std::vector<WasmEdge::ValVariant> SortArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(0)),
        WasmEdge::ValVariant(uint32_t(LEN - 1))};
    auto SortRes = Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
    ASSERT_TRUE(SortRes.has_value()) << "quicksort failed";

    // Verify sorted with is_sorted
    std::vector<WasmEdge::ValVariant> CheckArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(LEN))};
    auto CheckRes = Executor.invoke(IsSortedFunc, CheckArgs, IsSortedParams);
    ASSERT_TRUE(CheckRes.has_value());
    EXPECT_EQ((*CheckRes)[0].first.get<uint32_t>(), 1u) << "Array should be sorted";

    // Also verify manually
    int32_t expected[] = {1, 2, 5, 8, 9};
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      int32_t val = *reinterpret_cast<int32_t *>(ptr);
      EXPECT_EQ(val, expected[i]) << "Element at index " << i << " incorrect";
    }
  }

  // Test case 2: Already sorted array
  {
    const uint32_t BASE = 100;
    const uint32_t LEN = 4;
    int32_t testData[] = {1, 2, 3, 4};
    
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      *reinterpret_cast<int32_t *>(ptr) = testData[i];
    }

    std::vector<WasmEdge::ValVariant> SortArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(0)),
        WasmEdge::ValVariant(uint32_t(LEN - 1))};
    auto SortRes = Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
    ASSERT_TRUE(SortRes.has_value());

    std::vector<WasmEdge::ValVariant> CheckArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(LEN))};
    auto CheckRes = Executor.invoke(IsSortedFunc, CheckArgs, IsSortedParams);
    ASSERT_TRUE(CheckRes.has_value());
    EXPECT_EQ((*CheckRes)[0].first.get<uint32_t>(), 1u) << "Already sorted array should remain sorted";
  }

  // Test case 3: Reverse sorted array
  {
    const uint32_t BASE = 200;
    const uint32_t LEN = 6;
    int32_t testData[] = {6, 5, 4, 3, 2, 1};
    
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      *reinterpret_cast<int32_t *>(ptr) = testData[i];
    }

    std::vector<WasmEdge::ValVariant> SortArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(0)),
        WasmEdge::ValVariant(uint32_t(LEN - 1))};
    auto SortRes = Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
    ASSERT_TRUE(SortRes.has_value());

    std::vector<WasmEdge::ValVariant> CheckArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(LEN))};
    auto CheckRes = Executor.invoke(IsSortedFunc, CheckArgs, IsSortedParams);
    ASSERT_TRUE(CheckRes.has_value());
    EXPECT_EQ((*CheckRes)[0].first.get<uint32_t>(), 1u) << "Reverse array should be sorted";
  }

  // Test case 4: Single element (edge case)
  {
    const uint32_t BASE = 300;
    uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE);
    *reinterpret_cast<int32_t *>(ptr) = 42;

    std::vector<WasmEdge::ValVariant> SortArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(0)),
        WasmEdge::ValVariant(uint32_t(0))};
    auto SortRes = Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
    ASSERT_TRUE(SortRes.has_value());

    std::vector<WasmEdge::ValVariant> CheckArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(1))};
    auto CheckRes = Executor.invoke(IsSortedFunc, CheckArgs, IsSortedParams);
    ASSERT_TRUE(CheckRes.has_value());
    EXPECT_EQ((*CheckRes)[0].first.get<uint32_t>(), 1u) << "Single element should be sorted";
  }

  // Test case 5: Array with duplicates
  {
    const uint32_t BASE = 400;
    const uint32_t LEN = 7;
    int32_t testData[] = {3, 1, 4, 1, 5, 9, 2};
    
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      *reinterpret_cast<int32_t *>(ptr) = testData[i];
    }

    std::vector<WasmEdge::ValVariant> SortArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(0)),
        WasmEdge::ValVariant(uint32_t(LEN - 1))};
    auto SortRes = Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
    ASSERT_TRUE(SortRes.has_value());

    std::vector<WasmEdge::ValVariant> CheckArgs = {
        WasmEdge::ValVariant(uint32_t(BASE)),
        WasmEdge::ValVariant(uint32_t(LEN))};
    auto CheckRes = Executor.invoke(IsSortedFunc, CheckArgs, IsSortedParams);
    ASSERT_TRUE(CheckRes.has_value());
    EXPECT_EQ((*CheckRes)[0].first.get<uint32_t>(), 1u) << "Array with duplicates should be sorted";
  }
}

TEST_F(IRBenchmarkTest, Benchmark_Quicksort) {
  auto TestDataPath = getTestDataPath();
  auto WasmPath = TestDataPath / "fibonacci.wasm";

  if (!std::filesystem::exists(WasmPath)) {
    GTEST_SKIP() << "Test data not found";
  }

  WasmEdge::Loader::Loader Loader(Conf);
  auto LoadRes = Loader.parseModule(WasmPath);
  ASSERT_TRUE(LoadRes.has_value());

  WasmEdge::Validator::Validator Validator(Conf);
  ASSERT_TRUE(Validator.validate(**LoadRes).has_value());

  WasmEdge::Runtime::StoreManager StoreMgr;
  WasmEdge::Executor::Executor Executor(Conf);

  auto InstRes = Executor.registerModule(StoreMgr, **LoadRes, "bench");
  ASSERT_TRUE(InstRes.has_value());

  auto *ModInst = InstRes->get();
  auto *QuicksortFunc = ModInst->findFuncExports("quicksort");
  ASSERT_NE(QuicksortFunc, nullptr);

  auto MemInsts = ModInst->getMemoryInstances();
  ASSERT_FALSE(MemInsts.empty());
  auto *MemInst = MemInsts[0];

  auto QuicksortParams = QuicksortFunc->getFuncType().getParamTypes();

  const uint32_t BASE = 0;
  const uint32_t LEN = 100;
  const int ITERATIONS = 1000;

  // Generate reverse-sorted array (worst case for naive quicksort, but our Lomuto is decent)
  auto initArray = [&]() {
    for (uint32_t i = 0; i < LEN; i++) {
      uint8_t *ptr = MemInst->getPointer<uint8_t *>(BASE + i * 4);
      *reinterpret_cast<int32_t *>(ptr) = static_cast<int32_t>(LEN - i);
    }
  };

  std::vector<WasmEdge::ValVariant> SortArgs = {
      WasmEdge::ValVariant(uint32_t(BASE)),
      WasmEdge::ValVariant(uint32_t(0)),
      WasmEdge::ValVariant(uint32_t(LEN - 1))};

  double avgTime = measureTime(
      [&]() {
        initArray();  // Reset array before each sort
        Executor.invoke(QuicksortFunc, SortArgs, QuicksortParams);
      },
      ITERATIONS);

  std::cout << "\n=== Quicksort Benchmark ===" << std::endl;
  std::cout << "  quicksort(" << LEN << " elements) x " << ITERATIONS << " iterations" << std::endl;
  std::cout << "  JIT: " << QuicksortFunc->isIRJitFunction() << std::endl;
  std::cout << "  Avg time per call: " << std::fixed << std::setprecision(3)
            << avgTime << " ms" << std::endl;
  std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
            << (1000.0 / avgTime) << " sorts/sec" << std::endl;
}

// ============================================================================
// Sightglass Benchmark Suite: Interpreter vs JIT, metrics table
// ============================================================================
TEST_F(IRBenchmarkTest, SightglassSuite) {
  auto TestDataPath = getTestDataPath();
  auto SightglassDir = TestDataPath / "sightglass";

  if (!std::filesystem::exists(SightglassDir) ||
      !std::filesystem::is_directory(SightglassDir)) {
    GTEST_SKIP() << "Sightglass testdata not found. Run utils/download_sightglass.sh";
  }

  // Discover all .wasm kernels (run all in testdata, not just preferred list)
  std::vector<std::filesystem::path> kernels;
  for (const auto &e : std::filesystem::directory_iterator(SightglassDir)) {
    if (e.path().extension() == ".wasm")
      kernels.push_back(e.path());
  }
  std::sort(kernels.begin(), kernels.end());

  // Optional: for fast CI / ctest, drop only the heaviest kernels (full list: WASMEDGE_SIGHTGLASS_QUICK=0).
  const char *quickEnv = std::getenv("WASMEDGE_SIGHTGLASS_QUICK");
  if (quickEnv && quickEnv[0] == '1') {
    std::vector<std::filesystem::path> filtered;
    for (const auto &p : kernels) {
      const std::string stem = p.stem().string();
      if (stem.compare(0, 13, "spidermonkey-") == 0)
        continue;
      if (stem.compare(0, 7, "tinygo-") == 0)
        continue;
      filtered.push_back(p);
    }
    kernels = std::move(filtered);
  }

  // Optional: run only one kernel and/or one mode (for isolation when checking which pass/fail/crash)
  const char *singleEnv = std::getenv("WASMEDGE_SIGHTGLASS_KERNEL");
  if (singleEnv && singleEnv[0] != '\0') {
    std::string single(singleEnv);
    if (single.size() < 5 || single.compare(single.size() - 5, 5, ".wasm") != 0)
      single += ".wasm";
    auto it = std::remove_if(kernels.begin(), kernels.end(), [&single](const std::filesystem::path &p) {
      return p.filename().string() != single && p.stem().string() != single;
    });
    kernels.erase(it, kernels.end());
  }

  const char *modeEnv = std::getenv("WASMEDGE_SIGHTGLASS_MODE");
  bool interpOnly = (modeEnv && (std::strcmp(modeEnv, "Interpreter") == 0));
  bool jitOnly = (modeEnv && (std::strcmp(modeEnv, "JIT") == 0));
  bool aotOnly = (modeEnv && (std::strcmp(modeEnv, "AOT") == 0));
  bool irJitOnly = (modeEnv && (std::strcmp(modeEnv, "IR_JIT") == 0));
  const char *skipInterpEnv = std::getenv("WASMEDGE_SIGHTGLASS_SKIP_INTERP");
  bool skipInterp = (skipInterpEnv && skipInterpEnv[0] == '1');
  const char *skipLlvmJitEnv = std::getenv("WASMEDGE_SIGHTGLASS_SKIP_LLVM_JIT");
  bool skipLlvmJit = (skipLlvmJitEnv && skipLlvmJitEnv[0] == '1');

  if (kernels.empty()) {
    GTEST_SKIP() << "No .wasm kernels in sightglass/. Run utils/download_sightglass.sh";
  }

  using namespace std::chrono;
  using Micro = duration<double, std::micro>;

  // Table header
  std::cout << "\n=== Sightglass IR JIT Benchmark Suite ===" << std::endl;
  std::cout << std::left << std::setw(20) << "Kernel"
            << std::setw(14) << "Mode"
            << std::setw(18) << "Inst.Lat(µs)"
            << std::setw(18) << "WorkTime(µs)"
            << std::setw(18) << "TtV(µs)"
            << std::endl;
  std::cout << std::string(88, '-') << std::endl;

  for (const auto &wasmPath : kernels) {
    std::string kernelName = wasmPath.stem().string();
    std::cout << "Running kernel: " << kernelName << std::endl;
    StdioCapture interpCap, jitCap, irJitCap, aotCap;
    bool interpOk = false;
    bool jitOk = false;
    bool irJitOk = false;
    bool aotOk = false;

    // Helper: load .input files for this kernel into a WasiStubModule's virtual FS
    auto loadVirtualFiles = [&](WasiStubModule &stub) {
      for (const auto &e : std::filesystem::directory_iterator(SightglassDir)) {
        auto fname = e.path().filename().string();
        // Match files like "shootout-ackermann.m.input" for kernel "shootout-ackermann"
        if (fname.size() > kernelName.size() + 1 &&
            fname.substr(0, kernelName.size()) == kernelName &&
            fname.find(".input") != std::string::npos) {
          std::ifstream ifs(e.path(), std::ios::binary);
          if (ifs) {
            std::string content((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
            stub.addVirtualFile(fname, std::move(content));
          }
        }
      }
      // Map kernel-prefixed default inputs to names benchmarks expect (default.input / default.input.md)
      auto loadAs = [&](const std::string &prefixedName, const std::string &virtualName) {
        auto path = SightglassDir / prefixedName;
        if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path))
          return;
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return;
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        stub.addVirtualFile(virtualName, std::move(content));
      };
      loadAs(kernelName + ".default.input", "default.input");
      loadAs(kernelName + ".default.input.md", "default.input.md");
      loadAs(kernelName + ".secret.der", "secret.der");
      loadAs(kernelName + ".small.input", "small.input");
      // Legacy: pulldown-cmark also accepts default.input.md without prefix
      if (kernelName == "pulldown-cmark")
        loadAs("default.input.md", "default.input.md");
    };

    const char *modes[] = {"Interpreter", "IR_JIT", "JIT"};
    for (const char *modeName : modes) {
      if (skipInterp && std::strcmp(modeName, "Interpreter") == 0) continue;
      if (skipLlvmJit && std::strcmp(modeName, "JIT") == 0) continue;
      if (interpOnly && std::strcmp(modeName, "Interpreter") != 0) continue;
      if (jitOnly && std::strcmp(modeName, "JIT") != 0) continue;
      if (irJitOnly && std::strcmp(modeName, "IR_JIT") != 0) continue;
      if (aotOnly) continue;
      bool useLlvmJIT = (std::strcmp(modeName, "JIT") == 0);
      bool useIRJIT = (std::strcmp(modeName, "IR_JIT") == 0);
      StdioCapture *cap = useLlvmJIT ? &jitCap : (useIRJIT ? &irJitCap : &interpCap);

      WasmEdge::Configure Conf;
      Conf.getCompilerConfigure().setOptimizationLevel(
          WasmEdge::CompilerConfigure::OptimizationLevel::O0);
      // Interpreter: ForceInterpreter=true,  EnableJIT=false
      // JIT (LLVM):  ForceInterpreter=false, EnableJIT=true
      // IR_JIT:      ForceInterpreter=false, EnableJIT=false (IR JIT runs in Executor::instantiate)
      Conf.getRuntimeConfigure().setForceInterpreter(!useLlvmJIT && !useIRJIT);
      Conf.getRuntimeConfigure().setEnableJIT(useLlvmJIT);

      WasiStubModule wasiStub(cap);
      loadVirtualFiles(wasiStub);
      BenchEnv benchEnv;
      BenchModule benchMod(benchEnv);

      WasmEdge::VM::VM VM(Conf);
      ASSERT_TRUE(VM.registerModule(wasiStub));
      ASSERT_TRUE(VM.registerModule(benchMod));

      auto ttvStart = high_resolution_clock::now();
      auto loadRes = VM.loadWasm(wasmPath);
      if (!loadRes) {
        std::cerr << "loadWasm failed: " << kernelName << " " << modeName
                  << " " << WasmEdge::ErrCode(loadRes.error()) << std::endl;
        continue;
      }
      ASSERT_TRUE(VM.validate());

      auto instStart = high_resolution_clock::now();
      auto instRes = VM.instantiate();
      auto instEnd = high_resolution_clock::now();
      if (!instRes) {
        std::cerr << "instantiate failed: " << kernelName << " " << modeName
                  << " " << WasmEdge::ErrCode(instRes.error()) << std::endl;
        continue;
      }

      double instLatencyUs = duration_cast<Micro>(instEnd - instStart).count();

      std::vector<WasmEdge::ValVariant> noArgs;
      std::vector<WasmEdge::ValType> noTypes;
      auto execRes = VM.execute("_start", noArgs, noTypes);
      auto ttvEnd = high_resolution_clock::now();
      double ttvUs = duration_cast<Micro>(ttvEnd - ttvStart).count();

      if (!execRes) {
        // WASI proc_exit triggers Terminated; that is normal success.
        if (execRes.error() != WasmEdge::ErrCode::Value::Terminated) {
          std::cerr << "execute _start failed: " << kernelName << " " << modeName
                    << " " << WasmEdge::ErrCode(execRes.error()) << std::endl;
          continue;
        }
      }

      if (useLlvmJIT) jitOk = true;
      else if (useIRJIT) irJitOk = true;
      else interpOk = true;

      double workTimeUs =
          duration_cast<Micro>(benchEnv.workTimeNs).count();

      std::cout << std::left << std::setw(20) << kernelName
                << std::setw(14) << modeName
                << std::fixed << std::setprecision(2)
                << std::setw(18) << instLatencyUs
                << std::setw(18) << workTimeUs
                << std::setw(18) << ttvUs
                << std::endl;
    }

#ifdef WASMEDGE_USE_LLVM
    // AOT mode (LLVM: compile wasm -> .so, then load .so and run).
    // Skip if WASMEDGE_SIGHTGLASS_SKIP_AOT=1 (e.g. for faster runs without LLVM AOT).
    const char *skipAotEnv = std::getenv("WASMEDGE_SIGHTGLASS_SKIP_AOT");
    const bool skipAOT = (skipAotEnv && skipAotEnv[0] == '1');
    if (!skipAOT && (aotOnly || (!interpOnly && !jitOnly && !irJitOnly))) {
      const char *modeName = "AOT";
      WasmEdge::Configure compileConf;
      compileConf.getCompilerConfigure().setOptimizationLevel(
          WasmEdge::CompilerConfigure::OptimizationLevel::O0);
      compileConf.getCompilerConfigure().setOutputFormat(
          WasmEdge::CompilerConfigure::OutputFormat::Native);
      compileConf.getRuntimeConfigure().setForceInterpreter(true);

      auto loadData = WasmEdge::Loader::Loader::loadFile(wasmPath);
      if (!loadData) {
        std::cerr << "AOT loadFile failed: " << kernelName
                  << " " << WasmEdge::ErrCode(loadData.error()) << std::endl;
      } else {
        WasmEdge::Loader::Loader loader(compileConf);
        auto parseRes = loader.parseModule(*loadData);
        if (!parseRes) {
          std::cerr << "AOT parseModule failed: " << kernelName << std::endl;
        } else {
          std::unique_ptr<WasmEdge::AST::Module> module = std::move(*parseRes);
          WasmEdge::Validator::Validator validator(compileConf);
          if (!validator.validate(*module)) {
            std::cerr << "AOT validate failed: " << kernelName << std::endl;
          } else {
            WasmEdge::LLVM::Compiler compiler(compileConf);
            if (!compiler.checkConfigure()) {
              std::cerr << "AOT checkConfigure failed: " << kernelName << std::endl;
            } else {
              auto compileRes = compiler.compile(*module);
              if (!compileRes) {
                std::cerr << "AOT compile failed: " << kernelName
                          << " " << WasmEdge::ErrCode(compileRes.error()) << std::endl;
              } else {
                std::filesystem::path soPath = std::filesystem::temp_directory_path() /
                    ("wasmedge_sightglass_" + kernelName + WASMEDGE_LIB_EXTENSION);
                WasmEdge::LLVM::CodeGen codegen(compileConf);
                if (!codegen.codegen(WasmEdge::Span<const WasmEdge::Byte>(*loadData),
                                    std::move(*compileRes), soPath)) {
                  std::cerr << "AOT codegen failed: " << kernelName << std::endl;
                } else {
                  soPath = std::filesystem::absolute(soPath);
                  WasmEdge::Configure runConf;
                  runConf.getRuntimeConfigure().setForceInterpreter(false);
                  runConf.getRuntimeConfigure().setEnableJIT(false);

                  WasiStubModule wasiStub(&aotCap);
                  loadVirtualFiles(wasiStub);
                  BenchEnv benchEnv;
                  BenchModule benchMod(benchEnv);

                  WasmEdge::VM::VM VM(runConf);
                  ASSERT_TRUE(VM.registerModule(wasiStub));
                  ASSERT_TRUE(VM.registerModule(benchMod));

                  auto ttvStart = high_resolution_clock::now();
                  auto loadRes = VM.loadWasm(soPath);
                  if (!loadRes) {
                    std::cerr << "AOT loadWasm(so) failed: " << kernelName
                              << " " << WasmEdge::ErrCode(loadRes.error()) << std::endl;
                  } else {
                    ASSERT_TRUE(VM.validate());
                    auto instStart = high_resolution_clock::now();
                    auto instRes = VM.instantiate();
                    auto instEnd = high_resolution_clock::now();
                    if (!instRes) {
                      std::cerr << "AOT instantiate failed: " << kernelName
                                << " " << WasmEdge::ErrCode(instRes.error()) << std::endl;
                    } else {
                      double instLatencyUs = duration_cast<Micro>(instEnd - instStart).count();
                      std::vector<WasmEdge::ValVariant> noArgs;
                      std::vector<WasmEdge::ValType> noTypes;
                      auto execRes = VM.execute("_start", noArgs, noTypes);
                      auto ttvEnd = high_resolution_clock::now();
                      double ttvUs = duration_cast<Micro>(ttvEnd - ttvStart).count();

                      if (!execRes) {
                        if (execRes.error() != WasmEdge::ErrCode::Value::Terminated)
                          std::cerr << "AOT execute failed: " << kernelName << std::endl;
                        else
                          aotOk = true; // proc_exit = success
                      } else {
                        aotOk = true;
                      }
                      if (aotOk) {
                        double workTimeUs =
                            duration_cast<Micro>(benchEnv.workTimeNs).count();
                        std::cout << std::left << std::setw(20) << kernelName
                                  << std::setw(14) << modeName
                                  << std::fixed << std::setprecision(2)
                                  << std::setw(18) << instLatencyUs
                                  << std::setw(18) << workTimeUs
                                  << std::setw(18) << ttvUs
                                  << std::endl;
                      }
                    }
                  }
                  std::filesystem::remove(soPath);
                }
              }
            }
          }
        }
      }
    }
#endif

    // Correctness: every kernel must ship kernel.stdout.expected and kernel.stderr.expected
    // next to the .wasm; successful runs are compared for both streams (empty file => empty string).
    const auto goldenStdout = SightglassDir / (kernelName + ".stdout.expected");
    const auto goldenStderr = SightglassDir / (kernelName + ".stderr.expected");
    ASSERT_TRUE(std::filesystem::exists(goldenStdout))
        << "Missing golden " << goldenStdout << " (add or run utils/download_sightglass.sh)";
    ASSERT_TRUE(std::filesystem::exists(goldenStderr))
        << "Missing golden " << goldenStderr << " (add or run utils/download_sightglass.sh)";

    auto loadExpected = [&](const std::string &suffix) -> std::string {
      auto path = SightglassDir / (kernelName + suffix);
      std::ifstream ifs(path, std::ios::binary);
      if (!ifs) return "";
      return std::string((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());
    };

    const std::string expectedStdout = loadExpected(".stdout.expected");
    const std::string expectedStderr = loadExpected(".stderr.expected");

    auto checkExpected = [&](const std::string &label, const StdioCapture &cap, bool ok) {
      if (!ok) return;
      EXPECT_EQ(cap.stdout_, expectedStdout)
          << "Kernel " << kernelName << " (" << label << "): stdout mismatch vs .expected";
      EXPECT_EQ(cap.stderr_, expectedStderr)
          << "Kernel " << kernelName << " (" << label << "): stderr mismatch vs .expected";
    };
    checkExpected("Interpreter", interpCap, interpOk);
    checkExpected("JIT", jitCap, jitOk);
    checkExpected("IR_JIT", irJitCap, irJitOk);
    checkExpected("AOT", aotCap, aotOk);
  }
  std::cout << std::endl;
}

} // namespace

int main(int argc, char **argv) {
  int n = 0;
  bool quiet = false;
  if (std::getenv("WASMEDGE_QUIET") != nullptr) {
    quiet = true;
  }
  for (int i = 0; i < argc; ++i) {
    if (std::strcmp(argv[i], "--quiet") == 0 || std::strcmp(argv[i], "-q") == 0) {
      quiet = true;
    } else {
      argv[n++] = argv[i];
    }
  }
  argc = n;
  if (quiet) {
    WasmEdge::Log::setErrorLoggingLevel();
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
