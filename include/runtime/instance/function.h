// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/runtime/instance/function.h - Function Instance definition ==//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the function instance definition in store manager.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "ast/instruction.h"
#include "common/symbol.h"
#include "runtime/hostfunc.h"
#include "runtime/instance/composite.h"

#include <memory>
#include <numeric>
#include <string>
#include <vector>


namespace WasmEdge {
namespace Runtime {
namespace Instance {

class ModuleInstance;

class FunctionInstance : public CompositeBase {
public:
  using CompiledFunction = void;

  FunctionInstance() = delete;
  /// Move constructor.
  FunctionInstance(FunctionInstance &&Inst) noexcept
      : CompositeBase(Inst.ModInst, Inst.TypeIdx), FuncType(Inst.FuncType),
        Data(std::move(Inst.Data)) {
    assuming(ModInst);
  }
  /// Constructor for native function.
  FunctionInstance(const ModuleInstance *Mod, const uint32_t TIdx,
                   const AST::FunctionType &Type,
                   Span<const std::pair<uint32_t, ValType>> Locs,
                   AST::InstrView Expr) noexcept
      : CompositeBase(Mod, TIdx), FuncType(Type),
        Data(std::in_place_type_t<WasmFunction>(), Locs, Expr) {
    assuming(ModInst);
  }
  /// Constructor for compiled function.
  FunctionInstance(const ModuleInstance *Mod, const uint32_t TIdx,
                   const AST::FunctionType &Type,
                   Symbol<CompiledFunction> S) noexcept
      : CompositeBase(Mod, TIdx), FuncType(Type),
        Data(std::in_place_type_t<Symbol<CompiledFunction>>(), std::move(S)) {
    assuming(ModInst);
  }
  /// Constructors for host function.
  FunctionInstance(const ModuleInstance *Mod, const uint32_t TIdx,
                   std::unique_ptr<HostFunctionBase> &&Func) noexcept
      : CompositeBase(Mod, TIdx), FuncType(Func->getFuncType()),
        Data(std::in_place_type_t<std::unique_ptr<HostFunctionBase>>(),
             std::move(Func)) {
    assuming(ModInst);
  }
  FunctionInstance(std::unique_ptr<HostFunctionBase> &&Func) noexcept
      : CompositeBase(), FuncType(Func->getFuncType()),
        Data(std::in_place_type_t<std::unique_ptr<HostFunctionBase>>(),
             std::move(Func)) {}

#ifdef WASMEDGE_BUILD_IR_JIT
  /// Constructor for IR JIT compiled function.
  FunctionInstance(const ModuleInstance *Mod, const uint32_t TIdx,
                   const AST::FunctionType &Type, void *NativeFunc,
                   size_t CodeSize, std::string IRText) noexcept
      : CompositeBase(Mod, TIdx), FuncType(Type),
        Data(std::in_place_type_t<IRJitFunction>(), NativeFunc, CodeSize,
             std::move(IRText)) {
    assuming(ModInst);
  }
#endif

  /// Getter of checking is native wasm function.
  bool isWasmFunction() const noexcept {
    return std::holds_alternative<WasmFunction>(Data);
  }

  /// Getter of checking is compiled function.
  bool isCompiledFunction() const noexcept {
    return std::holds_alternative<Symbol<CompiledFunction>>(Data);
  }

  /// Getter of checking is host function.
  bool isHostFunction() const noexcept {
    return std::holds_alternative<std::unique_ptr<HostFunctionBase>>(Data);
  }

#ifdef WASMEDGE_BUILD_IR_JIT
  /// Getter of checking is IR JIT compiled function.
  bool isIRJitFunction() const noexcept {
    return std::holds_alternative<IRJitFunction>(Data);
  }
#endif

  /// Getter of function type.
  const AST::FunctionType &getFuncType() const noexcept { return FuncType; }

  /// Getter of function local variables.
  Span<const std::pair<uint32_t, ValType>> getLocals() const noexcept {
    return std::get_if<WasmFunction>(&Data)->Locals;
  }

  /// Getter of function local number.
  uint32_t getLocalNum() const noexcept {
    return std::get_if<WasmFunction>(&Data)->LocalNum;
  }

  /// Getter of function body instrs.
  AST::InstrView getInstrs() const noexcept {
    if (std::holds_alternative<WasmFunction>(Data)) {
      return std::get<WasmFunction>(Data).Instrs;
    } else {
      return {};
    }
  }

  /// Getter of symbol
  auto &getSymbol() const noexcept {
    return *std::get_if<Symbol<CompiledFunction>>(&Data);
  }

  /// Getter of host function.
  HostFunctionBase &getHostFunc() const noexcept {
    return *std::get_if<std::unique_ptr<HostFunctionBase>>(&Data)->get();
  }

private:
#ifdef WASMEDGE_BUILD_IR_JIT
  // Forward declare IR JIT function struct
  struct IRJitFunction;
#endif

public:
#ifdef WASMEDGE_BUILD_IR_JIT
  /// Getter of IR JIT function data.
  const IRJitFunction &getIRJitFunc() const noexcept {
    return *std::get_if<IRJitFunction>(&Data);
  }
  
  /// Getter of IR JIT native function pointer.
  void *getIRJitNativeFunc() const noexcept {
    if (auto *Func = std::get_if<IRJitFunction>(&Data)) {
      return Func->NativeFunc;
    }
    return nullptr;
  }
  
  /// Upgrade from WasmFunction to IR JIT compiled function.
  /// Returns true if successful, false if not a WasmFunction.
  bool upgradeToIRJit(void *NativeFunc, size_t CodeSize,
                      std::string IRText) noexcept {
    if (!std::holds_alternative<WasmFunction>(Data)) {
      return false;
    }
    Data.template emplace<IRJitFunction>(NativeFunc, CodeSize, std::move(IRText));
    return true;
  }
#endif

private:
  struct WasmFunction {
    const std::vector<std::pair<uint32_t, ValType>> Locals;
    const uint32_t LocalNum;
    AST::InstrVec Instrs;
    WasmFunction(Span<const std::pair<uint32_t, ValType>> Locs,
                 AST::InstrView Expr) noexcept
        : Locals(Locs.begin(), Locs.end()),
          LocalNum(
              std::accumulate(Locals.begin(), Locals.end(), UINT32_C(0),
                              [](uint32_t N, const auto &Pair) -> uint32_t {
                                return N + Pair.first;
                              })) {
      // FIXME: Modify the capacity to prevent from connection of 2 vectors.
      Instrs.reserve(Expr.size() + 1);
      Instrs.assign(Expr.begin(), Expr.end());
    }
  };

#ifdef WASMEDGE_BUILD_IR_JIT
  struct IRJitFunction {
    void *NativeFunc;    // Pointer to JIT compiled code
    size_t CodeSize;     // Size of compiled code
    std::string IRText;  // Serialized IR text (preserved for potential tier-up)

    IRJitFunction(void *Func, size_t Size, std::string Text) noexcept
        : NativeFunc(Func), CodeSize(Size), IRText(std::move(Text)) {}
  };
#endif

  /// \name Data of function instance.
  /// @{

  const AST::FunctionType &FuncType;
#ifdef WASMEDGE_BUILD_IR_JIT
  std::variant<WasmFunction, Symbol<CompiledFunction>,
               std::unique_ptr<HostFunctionBase>, IRJitFunction>
      Data;
#else
  std::variant<WasmFunction, Symbol<CompiledFunction>,
               std::unique_ptr<HostFunctionBase>>
      Data;
#endif
  /// @}
};

} // namespace Instance
} // namespace Runtime
} // namespace WasmEdge
