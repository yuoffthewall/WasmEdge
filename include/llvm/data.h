// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/llvm/data.h - Data class definition ----------------------===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file is the definition class of Data class.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "ast/module.h"
#include "common/configure.h"
#include "common/errcode.h"
#include "common/filesystem.h"
#include "common/span.h"

#include <mutex>

// Forward declare the LLVM-C opaque types used by tier-2 accessors below.
// We intentionally avoid `#include <llvm-c/Types.h>` here so that targets
// that depend on wasmedgeLLVM's public headers (e.g. test binaries) do not
// need llvm-c on their own include path.
extern "C" {
struct LLVMOpaqueModule;
struct LLVMOpaqueContext;
struct LLVMOrcOpaqueThreadSafeContext;
typedef struct LLVMOpaqueModule *LLVMModuleRef;
typedef struct LLVMOpaqueContext *LLVMContextRef;
typedef struct LLVMOrcOpaqueThreadSafeContext *LLVMOrcThreadSafeContextRef;
}

namespace WasmEdge::LLVM {

/// Holds llvm-relative runtime data, like llvm::Context, llvm::Module, etc.
class Data {
public:
  struct DataContext;
  Data() noexcept;
  ~Data() noexcept;
  Data(Data &&) noexcept;
  Data &operator=(Data &&) noexcept;
  DataContext &extract() noexcept { return *Context; }

  /// Return the raw LLVMModuleRef owned by this Data. Used by the tier-2
  /// JIT path which post-processes the compiled module (adds thunks) and
  /// hands it to ORC LLJIT. Ownership stays with Data until releaseModule().
  LLVMModuleRef getModuleRef() noexcept;
  /// Return the raw LLVMContextRef associated with this Data.
  LLVMContextRef getContextRef() noexcept;
  /// Release ownership of the LLVM module (the caller becomes responsible
  /// for disposing it). Used to hand the module to ORC LLJIT.
  LLVMModuleRef releaseModule() noexcept;
  /// Release ownership of the ORC ThreadSafeContext that owns this Data's
  /// LLVMContext. Used by the tier-2 JIT path which creates its own ORC
  /// LLJIT with absolute-symbol bindings and therefore cannot use the
  /// standard LLVM::JIT::load() entry point. Caller becomes responsible
  /// for disposing the TSContext.
  LLVMOrcThreadSafeContextRef releaseTSContext() noexcept;

private:
  std::unique_ptr<DataContext> Context;
  const Configure Conf;
};

} // namespace WasmEdge::LLVM
