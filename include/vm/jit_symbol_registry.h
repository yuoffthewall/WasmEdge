// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/vm/jit_symbol_registry.h - JIT symbol name→addr map ------===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared symbol registry used by:
///   - ir_builder.cpp (tier-1): registers symbols, sets up ir_loader
///   - tier2_compiler.cpp (tier-2): queries registry to resolve LLVM externals
///
//===----------------------------------------------------------------------===//
#pragma once

#ifdef WASMEDGE_BUILD_IR_JIT

#include <string>
#include <unordered_map>

namespace WasmEdge::VM {

/// Returns the global JIT symbol registry (name → address).
/// Populated by ir_builder.cpp on first use via ensureSymbolsRegistered().
const std::unordered_map<std::string, void *> &getJitSymbolRegistry();

} // namespace WasmEdge::VM

#endif // WASMEDGE_BUILD_IR_JIT
