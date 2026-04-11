// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

/// \file
/// Process-global registry mapping structurally-equal FunctionTypes to unique
/// canonical type IDs (uint32_t). Used by the IR JIT shadow dispatch table to
/// replace expensive structural type matching with a single integer compare.

#pragma once

#include "ast/type.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace WasmEdge {
namespace VM {

class CanonicalTypeRegistry {
public:
  /// Get or assign a canonical ID for the given function type.
  /// Thread-safe. Returns a non-zero ID (0 = invalid/unset).
  uint32_t getOrAssign(const AST::FunctionType &FT);

  /// Process-global singleton.
  static CanonicalTypeRegistry &instance();

private:
  struct FunctionTypeHash {
    size_t operator()(const AST::FunctionType &FT) const noexcept {
      size_t H = FT.getParamTypes().size() * 31 + FT.getReturnTypes().size();
      for (const auto &V : FT.getParamTypes())
        H = H * 37 + static_cast<size_t>(V.getCode());
      for (const auto &V : FT.getReturnTypes())
        H = H * 37 + static_cast<size_t>(V.getCode());
      return H;
    }
  };

  struct FunctionTypeEqual {
    bool operator()(const AST::FunctionType &A,
                    const AST::FunctionType &B) const noexcept {
      return A == B;
    }
  };

  std::mutex Mu;
  std::unordered_map<AST::FunctionType, uint32_t, FunctionTypeHash,
                     FunctionTypeEqual>
      Map;
  uint32_t NextId = 1; // 0 = invalid
};

} // namespace VM
} // namespace WasmEdge
