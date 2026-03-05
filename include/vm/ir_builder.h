// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

//===-- wasmedge/vm/ir_builder.h - IR Builder class definition ------------===//
//
// Part of the WasmEdge Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the declaration of the IR Builder class, which translates
/// WebAssembly bytecode to dstogov/ir graph representation.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "common/errcode.h"
#include "common/enum_ast.hpp"
#include "common/span.h"
#include "common/types.h"

#ifdef WASMEDGE_BUILD_IR_JIT

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

// Include IR library headers
extern "C" {
#include "ir.h"
}

namespace WasmEdge {

namespace AST {
class Instruction;
class FunctionType;
} // namespace AST

namespace VM {

/// IR Builder - translates WebAssembly to IR graph
class WasmToIRBuilder {
public:
  WasmToIRBuilder() noexcept;
  ~WasmToIRBuilder() noexcept;

  // Disable copy
  WasmToIRBuilder(const WasmToIRBuilder &) = delete;
  WasmToIRBuilder &operator=(const WasmToIRBuilder &) = delete;

  /// Initialize the builder with function signature
  Expect<void> initialize(const AST::FunctionType &FuncType,
                          Span<const std::pair<uint32_t, ValType>> Locals);

  /// Set module function types for call instructions
  /// Must be called before buildFromInstructions if the function uses call
  void setModuleFunctions(Span<const AST::FunctionType *> FuncTypes) noexcept {
    ModuleFuncTypes.assign(FuncTypes.begin(), FuncTypes.end());
  }
  
  /// Set module global types for global.get/set instructions
  /// Must be called before buildFromInstructions if the function uses globals
  void setModuleGlobals(Span<const ValType> GlobalTypes) noexcept {
    ModuleGlobalTypes.assign(GlobalTypes.begin(), GlobalTypes.end());
  }

  /// Build IR from WebAssembly instruction sequence
  Expect<void> buildFromInstructions(Span<const AST::Instruction> Instrs);

  /// Finalize and get the IR context
  ir_ctx *getIRContext() noexcept { return &Ctx; }

  /// Reset the builder for a new function
  void reset() noexcept;

private:
  /// Control flow construct type
  enum class ControlKind { Block, Loop, If };

  /// Label information for structured control flow
  struct LabelInfo {
    ControlKind Kind;          // Type of control construct
    ir_ref LoopHeader;         // For loops: LOOP_BEGIN reference
    ir_ref IfRef;              // For if: IF node reference
    ir_ref ElseEnd;            // For if: end of else branch (or true branch if no else)
    std::vector<ir_ref> EndList;  // List of ir_END() refs to merge at end
    std::vector<std::map<uint32_t, ir_ref>> EndLocals;  // Locals state at each EndList entry
    std::vector<ir_ref> BranchResults; // Result values from each branch (for result types)
    uint32_t Arity;            // Number of values expected at branch (0 or 1)
    ir_type ResultType;        // IR type of result (IR_VOID if no result)
    uint32_t StackBase;        // Stack base for this label
    bool InElseBranch;         // For if: are we in the else branch?
    bool HasElse;              // For if: does this have an else clause?
    bool TrueBranchTerminated; // For if: did true branch terminate (return/unreachable)?
    bool ElseBranchTerminated; // For if: did else branch terminate?
    
    // For loops: PHI nodes for local variables
    std::map<uint32_t, ir_ref> LoopLocalPhis;  // LocalIdx -> PHI node ref
    std::map<uint32_t, ir_ref> PreLoopLocals;  // LocalIdx -> value before loop
    
    // For if: locals state at the start of if (before any branch executes)
    std::map<uint32_t, ir_ref> PreIfLocals;    // LocalIdx -> value before if
  };

  /// Stack operations
  void push(ir_ref Value) noexcept { ValueStack.push_back(Value); }
  ir_ref pop() noexcept;
  ir_ref peek(uint32_t Depth = 0) const noexcept;

  /// Process single instruction
  Expect<void> visitInstruction(const AST::Instruction &Instr);

  /// Instruction type handlers
  Expect<void> visitUnary(OpCode Op);
  Expect<void> visitBinary(OpCode Op);
  Expect<void> visitCompare(OpCode Op);
  Expect<void> visitConversion(OpCode Op);
  Expect<void> visitParametric(const AST::Instruction &Instr);
  Expect<void> visitConst(const AST::Instruction &Instr);
  Expect<void> visitLocal(const AST::Instruction &Instr);
  Expect<void> visitGlobal(const AST::Instruction &Instr);
  Expect<void> visitMemory(const AST::Instruction &Instr);
  Expect<void> visitControl(const AST::Instruction &Instr);
  Expect<void> visitCall(const AST::Instruction &Instr);

  /// Control flow helpers
  Expect<void> visitBlock(const AST::Instruction &Instr);
  Expect<void> visitLoop(const AST::Instruction &Instr);
  Expect<void> visitIf(const AST::Instruction &Instr);
  Expect<void> visitElse(const AST::Instruction &Instr);
  Expect<void> visitEnd(const AST::Instruction &Instr);
  Expect<void> visitBr(const AST::Instruction &Instr);
  Expect<void> visitBrIf(const AST::Instruction &Instr);
  Expect<void> visitBrTable(const AST::Instruction &Instr);
  Expect<void> visitReturn(const AST::Instruction &Instr);

  /// Memory operation helpers
  ir_ref buildMemoryAddress(ir_ref Base, uint32_t Offset);
  ir_ref buildBoundsCheck(ir_ref Address, uint32_t AccessSize);

  /// Type conversion
  ir_type wasmTypeToIRType(ValType Type) const noexcept;

  /// Return value for ir_RETURN: pop if stack has value, else emit constant matching ret_type.
  /// Ensures we never emit IR_UNUSED for a value-returning function (avoids IR assertion).
  ir_ref getOrEmitReturnValue() noexcept;

private:
  ir_ctx Ctx;                               // IR context (stack allocated)
  bool Initialized;                         // Track if context is initialized
  bool CurrentPathTerminated;               // True if current code path hit return/unreachable
  std::vector<ir_ref> ValueStack;           // Wasm value stack (SSA values)
  std::map<uint32_t, ir_ref> Locals;        // Local variables
  std::vector<LabelInfo> LabelStack;        // Control flow label stack
  ir_ref FuncTablePtr;                      // Pointer to function table (for calls)
  ir_ref FuncTableSize;                     // Size of function table (for call_indirect bounds check)
  ir_ref GlobalBasePtr;                     // Pointer to globals array (ValVariant*)
  ir_ref MemoryBase;                        // Pointer to linear memory base
  ir_ref MemorySize;                        // Current memory size (in bytes)
  uint32_t LocalCount;                      // Total number of locals
  
  // Module-level information for function calls
  std::vector<const AST::FunctionType *> ModuleFuncTypes;  // Function types for all module functions
  
  // Module-level information for globals
  std::vector<ValType> ModuleGlobalTypes;  // Types of all module globals
};

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT

