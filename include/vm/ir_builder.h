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
#include <utility>
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

  /// Set the number of imported functions. Indices < ImportFuncNum are imports
  /// and call to them will go through the host call trampoline.
  void setImportFuncNum(uint32_t Num) noexcept { ImportFuncNum = Num; }

  /// Set which functions are skipped (not JIT-compiled). Calls to these must
  /// go through the host call trampoline instead of direct inline calls.
  void setSkippedFunctions(const std::vector<bool> &Skip) {
    SkippedFunctions = Skip;
  }

  /// Set the max number of call arguments across all calls in the function.
  /// Used to pre-allocate a shared args buffer in the function prologue.
  void setMaxCallArgs(uint32_t N) noexcept { MaxCallArgs = N; }

  /// Set module types from the type section (indexed by type index).
  /// Used for call_indirect type resolution.
  void setModuleTypes(Span<const AST::FunctionType *> Types) noexcept {
    ModuleTypeSection.assign(Types.begin(), Types.end());
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
    std::vector<std::pair<ir_ref, ir_ref>> RefBranchResults; // When result is ref: (type, ptr) per branch
    uint32_t Arity;            // Number of values expected at branch (0 or 1)
    ir_type ResultType;        // IR type of result (IR_VOID if no result)
    bool ResultIsRef = false;  // True when block/if result type is ref (two stack slots)
    uint32_t StackBase;        // Stack base for this label
    bool InElseBranch;         // For if: are we in the else branch?
    bool HasElse;              // For if: does this have an else clause?
    bool TrueBranchTerminated; // For if: did true branch terminate (return/unreachable)?
    bool ElseBranchTerminated; // For if: did else branch terminate?
    
    // For loops: PHI nodes for local variables
    std::map<uint32_t, ir_ref> LoopLocalPhis;  // LocalIdx -> PHI node ref
    std::map<uint32_t, ir_ref> PreLoopLocals;  // LocalIdx -> value before loop
    bool BackEdgeEmitted = false;              // Has a loop back-edge been emitted already?

    // For loops: collected back-edge ENDs (finalized at loop end)
    std::vector<ir_ref> LoopBackEdgeEnds;
    std::vector<std::map<uint32_t, ir_ref>> LoopBackEdgeLocals;
    
    // For if: locals state at the start of if (before any branch executes)
    std::map<uint32_t, ir_ref> PreIfLocals;    // LocalIdx -> value before if
  };

  /// Stack operations
  void push(ir_ref Value) noexcept { ValueStack.push_back(Value); }
  ir_ref pop() noexcept;
  ir_ref peek(uint32_t Depth = 0) const noexcept;

  /// Ref (RefVariant) as two stack slots: type (first), ptr (second). Match RefVariant layout.
  void pushRef(ir_ref TypeRef, ir_ref PtrRef) noexcept {
    push(TypeRef);
    push(PtrRef);
  }
  /// Pops ptr then type (reverse of pushRef). Returns (PtrRef, TypeRef).
  std::pair<ir_ref, ir_ref> popRef() noexcept {
    ir_ref PtrRef = pop();
    ir_ref TypeRef = pop();
    return {PtrRef, TypeRef};
  }

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
  Expect<void> visitRefType(const AST::Instruction &Instr);
  Expect<void> visitTable(const AST::Instruction &Instr);

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
  /// Emit a bounds check when WASMEDGE_IR_JIT_BOUND_CHECK=1 (default off): traps
  /// if (uint64_t)Base + Offset + AccessSize exceeds the current linear memory size.
  /// Must be called before the load/store.
  void buildBoundsCheck(ir_ref Base, uint32_t Offset, uint32_t AccessSize);

  /// Type conversion
  ir_type wasmTypeToIRType(ValType Type) const noexcept;

  /// Return value for ir_RETURN: pop if stack has value, else emit constant matching ret_type.
  /// Ensures we never emit IR_UNUSED for a value-returning function (avoids IR assertion).
  ir_ref getOrEmitReturnValue() noexcept;
  
  /// Ensure ir_ref is valid; if not, emit a zero constant of the given type.
  /// Used to sanitize values before passing to ir_CALL_N to avoid backend assertions.
  ir_ref ensureValidRef(ir_ref Ref, ir_type Type) noexcept;
  
  /// Coerce a value to a target type, emitting conversion if needed.
  /// Returns the original value if types are compatible, or a converted value.
  ir_ref coerceToType(ir_ref Value, ir_type TargetType) noexcept;

  /// Look up a local's IR type. Asserts the local exists in LocalTypes.
  ir_type getLocalType(uint32_t LocalIdx) const noexcept;

  /// Emit a PHI node for 2+ values. Uses ir_PHI_2 for 2, ir_PHI_N for >2.
  ir_ref emitPhi(ir_type Type, std::vector<ir_ref> &Vals);

  /// Merge locals from multiple control flow paths, creating PHI nodes where
  /// values differ. Returns the merged locals map.
  std::map<uint32_t, ir_ref> mergeLocals(
      const std::vector<std::map<uint32_t, ir_ref>> &EndLocals);

  /// Merge result values from multiple branches, creating a PHI and pushing it.
  void mergeResults(const std::vector<ir_ref> &BranchResults,
                    ir_type ResultType);

  /// Merge ref-typed result values (type, ptr pairs), creating two PHIs and pushing.
  void mergeRefResults(
      const std::vector<std::pair<ir_ref, ir_ref>> &RefBranchResults);

  /// Finalize a Block/If merge: emit MERGE, merge locals, merge results.
  void finalizeMerge(LabelInfo &Label);

  /// Emit a loop back-edge, merging with any previously emitted back-edge.
  void emitLoopBackEdge(LabelInfo &Target);

  /// Full function instruction stream and index of the opcode being visited
  /// (for static analysis of loop bodies; JumpEnd is relative to this array).
  Span<const AST::Instruction> FuncInstrs;
  uint32_t CurInstrIdx = 0;

  ir_ctx Ctx;                               // IR context (stack allocated)
  bool Initialized;                         // Track if context is initialized
  bool CurrentPathTerminated;               // True if current code path hit return/unreachable
  std::vector<ir_ref> ValueStack;           // Wasm value stack (SSA values)
  std::map<uint32_t, ir_ref> Locals;        // Local variables
  std::map<uint32_t, ir_type> LocalTypes;   // Types of local variables (fixed)
  std::vector<LabelInfo> LabelStack;        // Control flow label stack
  ir_ref EnvPtr;                            // JitExecEnv* (param 1)
  ir_ref FuncTablePtr;                      // Loaded from EnvPtr
  ir_ref FuncTableSize;                     // Loaded from EnvPtr
  ir_ref GlobalBasePtr;                     // Loaded from EnvPtr
  ir_ref MemoryBase;                        // Loaded from EnvPtr
  ir_ref HostCallFnPtr;                     // jit_host_call address from EnvPtr
  ir_ref DirectOrHostFnPtr;                 // jit_direct_or_host (null-safe call) from EnvPtr
  ir_ref MemoryGrowFnPtr;                   // jit_memory_grow from EnvPtr
  ir_ref MemorySizeFnPtr;                   // jit_memory_size from EnvPtr
  ir_ref CallIndirectFnPtr;                 // jit_call_indirect from EnvPtr
  ir_ref ArgsPtr;                           // uint64_t* args (param 2)
  ir_ref MemorySize;                        // Current memory size (in bytes)
  uint32_t LocalCount;                      // Total number of locals
  uint32_t CurrFuncNumParams = 0;           // Current function's parameter count (for call_indirect table-index fix)

  // Module-level information for function calls
  std::vector<const AST::FunctionType *> ModuleFuncTypes;  // Function types indexed by module function index
  std::vector<const AST::FunctionType *> ModuleTypeSection; // Function types indexed by type index
  uint32_t ImportFuncNum = 0;  // Number of imported functions
  std::vector<bool> SkippedFunctions;  // Functions skipped during JIT compilation
  
  // Pre-allocated args buffer for call instructions (avoids per-call ir_ALLOCA)
  uint32_t MaxCallArgs = 0;    // Max parameter count across all calls in function
  ir_ref SharedCallArgs;       // Pre-allocated buffer: uint64_t[MaxCallArgs]
  
  // Module-level information for globals
  std::vector<ValType> ModuleGlobalTypes;  // Types of all module globals
};

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT

