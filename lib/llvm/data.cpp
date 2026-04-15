// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "llvm/data.h"
#include "data.h"
#include "llvm.h"

namespace LLVM = WasmEdge::LLVM;

LLVM::Data::Data() noexcept : Context(std::make_unique<DataContext>()) {}

LLVM::Data::~Data() noexcept {}

LLVM::Data::Data(LLVM::Data &&RHS) noexcept : Context(std::move(RHS.Context)) {}
LLVM::Data &LLVM::Data::operator=(LLVM::Data &&RHS) noexcept {
  using std::swap;
  swap(Context, RHS.Context);
  return *this;
}

LLVMModuleRef LLVM::Data::getModuleRef() noexcept {
  return Context ? Context->LLModule.unwrap() : nullptr;
}

LLVMContextRef LLVM::Data::getContextRef() noexcept {
  return Context ? Context->getLLContext().unwrap() : nullptr;
}

LLVMModuleRef LLVM::Data::releaseModule() noexcept {
  return Context ? Context->LLModule.release() : nullptr;
}

LLVMOrcThreadSafeContextRef LLVM::Data::releaseTSContext() noexcept {
  if (!Context) {
    return nullptr;
  }
#if LLVM_VERSION_MAJOR >= 21
  // For LLVM >= 21 the Data stores a raw LLContext and materializes a
  // TSContext on demand. Materialize once and release.
  auto TS = Context->getTSContext();
  return TS.release();
#else
  return Context->TSContext.release();
#endif
}
