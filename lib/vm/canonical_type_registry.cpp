// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/canonical_type_registry.h"

namespace WasmEdge {
namespace VM {

uint32_t CanonicalTypeRegistry::getOrAssign(const AST::FunctionType &FT) {
  std::lock_guard<std::mutex> Lock(Mu);
  auto It = Map.find(FT);
  if (It != Map.end())
    return It->second;
  uint32_t Id = NextId++;
  Map.emplace(FT, Id);
  return Id;
}

CanonicalTypeRegistry &CanonicalTypeRegistry::instance() {
  static CanonicalTypeRegistry Registry;
  return Registry;
}

} // namespace VM
} // namespace WasmEdge
