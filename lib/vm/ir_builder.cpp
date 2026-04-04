// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/ir_builder.h"

#ifdef WASMEDGE_BUILD_IR_JIT

#include "ast/instruction.h"
#include "ast/type.h"
#include "common/errcode.h"
#include "vm/ir_jit_engine.h"
#include "vm/ir_jit_reg_invoke.h"

// Include dstogov/ir headers
extern "C" {
#include "ir.h"
#include "ir_builder.h"
}

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_set>

namespace WasmEdge {
namespace VM {

// Helpers for Wasm div_u/rem_u (IR expects U32/U64 operands; stack has I32/I64).
// JIT code calls these by address so ir_check passes.
namespace {
uint32_t wasm_i32_div_u(uint32_t a, uint32_t b) {
  return b ? (a / b) : 0;  // Wasm traps on div-by-zero
}
uint32_t wasm_i32_rem_u(uint32_t a, uint32_t b) {
  return b ? (a % b) : 0;
}
uint64_t wasm_i64_div_u(uint64_t a, uint64_t b) {
  return b ? (a / b) : 0;
}
uint64_t wasm_i64_rem_u(uint64_t a, uint64_t b) {
  return b ? (a % b) : 0;
}

// Wasm f32/f64.copysign: magnitude from first stack operand, sign from second (top).
static float wasm_f32_copysign(float mag, float sign_src) {
  return std::copysignf(mag, sign_src);
}
static double wasm_f64_copysign(double mag, double sign_src) {
  return std::copysign(mag, sign_src);
}

/// Mark locals that have local.set / local.tee in this span. Uses JumpEnd /
/// JumpElse from the loader (offsets in the same contiguous instruction array).
static void collectLocalWritesInSpan(Span<const AST::Instruction> InstrSpan,
                                     std::unordered_set<uint32_t> &Out) {
  size_t I = 0;
  while (I < InstrSpan.size()) {
    const AST::Instruction &Instr = InstrSpan[I];
    OpCode Op = Instr.getOpCode();
    switch (Op) {
    case OpCode::Local__set:
    case OpCode::Local__tee:
      Out.insert(Instr.getTargetIndex());
      ++I;
      break;
    case OpCode::Block:
    case OpCode::Loop: {
      uint32_t JE = Instr.getJumpEnd();
      if (JE >= 1) {
        collectLocalWritesInSpan(InstrSpan.subspan(I + 1, JE - 1), Out);
      }
      I += static_cast<size_t>(JE) + 1;
      break;
    }
    case OpCode::If: {
      uint32_t JE = Instr.getJumpEnd();
      uint32_t JElse = Instr.getJumpElse();
      if (JElse == JE) {
        if (JE >= 1) {
          collectLocalWritesInSpan(InstrSpan.subspan(I + 1, JE - 1), Out);
        }
      } else {
        if (JElse >= 1) {
          collectLocalWritesInSpan(InstrSpan.subspan(I + 1, JElse - 1), Out);
        }
        if (JE > JElse + 1) {
          collectLocalWritesInSpan(
              InstrSpan.subspan(I + JElse + 1, JE - JElse - 1), Out);
        }
      }
      I += static_cast<size_t>(JE) + 1;
      break;
    }
    default:
      ++I;
      break;
    }
  }
}

// WASMEDGE_IR_JIT_BOUND_CHECK=1 enables IR JIT linear-memory bounds checks before
// each load/store; unset or any other value leaves them disabled (default).
static bool irJitBoundCheckEnabled() {
  const char *E = std::getenv("WASMEDGE_IR_JIT_BOUND_CHECK");
  return E && E[0] == '1' && E[1] == '\0';
}
}  // namespace

WasmToIRBuilder::WasmToIRBuilder() noexcept
    : Initialized(false), CurrentPathTerminated(false), EnvPtr(0), FuncTablePtr(0), FuncTableSize(0), GlobalBasePtr(0), MemoryBase(0), HostCallFnPtr(0), DirectOrHostFnPtr(0), CallIndirectFnPtr(0), ArgsPtr(0), MemorySize(0), LocalCount(0), SharedCallArgs(0) {}

WasmToIRBuilder::~WasmToIRBuilder() noexcept { reset(); }

void WasmToIRBuilder::reset() noexcept {
  if (Initialized) {
    ir_free(&Ctx);
    Initialized = false;
  }
  CurrentPathTerminated = false;
  ValueStack.clear();
  Locals.clear();
  LocalTypes.clear();
  LabelStack.clear();
  EnvPtr = 0;
  FuncTablePtr = 0;
  FuncTableSize = 0;
  GlobalBasePtr = 0;
  MemoryBase = 0;
  HostCallFnPtr = 0;
  DirectOrHostFnPtr = 0;
  CallIndirectFnPtr = 0;
  ArgsPtr = 0;
  MemorySize = 0;
  LocalCount = 0;
  CurrFuncNumParams = 0;
  MaxCallArgs = 0;
  SharedCallArgs = 0;
  ModuleFuncTypes.clear();
  ModuleGlobalTypes.clear();
  FuncInstrs = Span<const AST::Instruction>();
  CurInstrIdx = 0;
}

Expect<void> WasmToIRBuilder::initialize(
    const AST::FunctionType &FuncType,
    Span<const std::pair<uint32_t, ValType>> LocalVars) {
  reset();

  // Initialize IR context. Default O2; override with WASMEDGE_IR_JIT_OPT_LEVEL=0|1 for debug.
  int ir_opt_level = 2;
  if (const char *e = std::getenv("WASMEDGE_IR_JIT_OPT_LEVEL")) {
    if (e[0] == '0' && e[1] == '\0')
      ir_opt_level = 0;
    else if (e[0] == '1' && e[1] == '\0')
      ir_opt_level = 1;
    else if (e[0] == '2' && e[1] == '\0')
      ir_opt_level = 2;
  }
  uint32_t ir_flags = IR_FUNCTION;
  if (ir_opt_level > 0) {
    ir_flags |= IR_OPT_FOLDING | IR_OPT_CFG | IR_OPT_CODEGEN;
    ir_flags |= IR_OPT_MEM2SSA;
  }
  if (ir_opt_level > 1) {
    ir_flags |= IR_OPT_INLINE;
  }
  // Use a generous initial constant-pool size (256) instead of the minimum (4).
  // The SCCP optimization pass in the IR library has a latent bug: it holds
  // a raw pointer into the instruction buffer while calling ir_const(), which
  // can trigger ir_grow_bottom() → realloc, invalidating the pointer.
  // Pre-allocating avoids the realloc during optimization.
  ir_init(&Ctx, ir_flags, 256, IR_INSNS_LIMIT_MIN);
  Initialized = true;

  // Set return type
  const auto &RetTypes = FuncType.getReturnTypes();
  if (!RetTypes.empty()) {
    Ctx.ret_type = wasmTypeToIRType(RetTypes[0]);
  } else {
    Ctx.ret_type = IR_VOID;
  }

  // Local variable for IR macros (they expect 'ctx')
  ir_ctx *ctx = &Ctx;

  // Start building IR function
  ir_START();

  // Set up function parameters as locals
  const auto &ParamTypes = FuncType.getParamTypes();
  CurrFuncNumParams = static_cast<uint32_t>(ParamTypes.size());

  // Calculate total locals
  LocalCount = ParamTypes.size();
  for (const auto &[Count, Type] : LocalVars) {
    LocalCount += Count;
  }

  // Hybrid calling convention based on parameter count:
  // ≤kRegCallMaxParams: register-based — func(env, p0, p1, ...)
  // >kRegCallMaxParams: buffer-based   — func(env, uint64_t *args)
  // Buffer-based preserves callee-saved registers for the function body.
  EnvPtr = ir_PARAM(IR_ADDR, "exec_env", 1);

  if (ParamTypes.size() <= kRegCallMaxParams) {
    // Register-based: each wasm param as a native-typed ir_PARAM.
    for (uint32_t i = 0; i < ParamTypes.size(); ++i) {
      ir_type irType = wasmTypeToIRType(ParamTypes[i]);
      char name[32];
      snprintf(name, sizeof(name), "p%u", i);
      Locals[i] = ir_PARAM(irType, name, i + 2);
      LocalTypes[i] = irType;
    }
    ArgsPtr = IR_UNUSED;
  } else {
    // Buffer-based: second param is pointer to uint64_t[] args buffer.
    ArgsPtr = ir_PARAM(IR_ADDR, "args", 2);
    // Load each wasm param from the buffer.
    for (uint32_t i = 0; i < ParamTypes.size(); ++i) {
      ir_type irType = wasmTypeToIRType(ParamTypes[i]);
      LocalTypes[i] = irType;
      ir_ref SlotAddr =
          ir_ADD_A(ArgsPtr, ir_CONST_ADDR(i * sizeof(uint64_t)));
      // Load as I64 from buffer, then truncate/bitcast to native type.
      if (irType == IR_I32) {
        Locals[i] = ir_LOAD_I32(SlotAddr);
      } else if (irType == IR_I64) {
        Locals[i] = ir_LOAD_I64(SlotAddr);
      } else if (irType == IR_FLOAT) {
        Locals[i] = ir_LOAD_F(SlotAddr);
      } else if (irType == IR_DOUBLE) {
        Locals[i] = ir_LOAD_D(SlotAddr);
      } else {
        Locals[i] = ir_LOAD_I64(SlotAddr);
      }
    }
  }

  FuncTablePtr = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, FuncTable))));
  FuncTableSize = ir_LOAD_U32(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, FuncTableSize))));
  GlobalBasePtr = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, GlobalBase))));
  MemoryBase = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, MemoryBase))));
  HostCallFnPtr = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, HostCallFn))));
  DirectOrHostFnPtr = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, DirectOrHostFn))));
  MemoryGrowFnPtr = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, MemoryGrowFn))));
  MemorySizeFnPtr = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, MemorySizeFn))));
  CallIndirectFnPtr = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, CallIndirectFn))));

  // Initialize additional locals to zero
  uint32_t localIdx = static_cast<uint32_t>(ParamTypes.size());
  for (const auto &[Count, Type] : LocalVars) {
    ir_type irType = wasmTypeToIRType(Type);
    for (uint32_t i = 0; i < Count; ++i) {
      LocalTypes[localIdx] = irType;  // Track local type
      // Initialize local to zero based on type
      if (irType == IR_I32) {
        Locals[localIdx++] = ir_CONST_I32(0);
      } else if (irType == IR_I64) {
        Locals[localIdx++] = ir_CONST_I64(0);
      } else if (irType == IR_FLOAT) {
        Locals[localIdx++] = ir_CONST_FLOAT(0.0f);
      } else if (irType == IR_DOUBLE) {
        Locals[localIdx++] = ir_CONST_DOUBLE(0.0);
      } else {
        Locals[localIdx++] = ir_CONST_I32(0);
      }
    }
  }

  return {};
}

Expect<void>
WasmToIRBuilder::buildFromInstructions(Span<const AST::Instruction> Instrs) {
  // Pre-pass: compute max callee arity for buffer-based calls.
  // Buffer path is used for: host calls (imports), call_indirect, and
  // direct JIT-to-JIT calls with >kRegCallMaxParams wasm parameters.
  uint32_t maxArity = 0;
  for (const auto &Instr : Instrs) {
    OpCode Op = Instr.getOpCode();
    if (Op == OpCode::Call) {
      uint32_t idx = Instr.getTargetIndex();
      if (idx < ModuleFuncTypes.size()) {
        uint32_t n =
            static_cast<uint32_t>(ModuleFuncTypes[idx]->getParamTypes().size());
        bool isHost = (idx < ImportFuncNum);
        bool isSkipped = (idx < SkippedFunctions.size() && SkippedFunctions[idx]);
        bool isHighArity = (n > kRegCallMaxParams);
        // Buffer needed for host calls, skipped functions, or high-arity JIT calls.
        if ((isHost || isSkipped || isHighArity) && n > maxArity)
          maxArity = n;
      }
    } else if (Op == OpCode::Call_indirect) {
      uint32_t typeIdx = Instr.getTargetIndex();
      if (typeIdx < ModuleTypeSection.size()) {
        uint32_t n = static_cast<uint32_t>(
            ModuleTypeSection[typeIdx]->getParamTypes().size());
        if (n > maxArity)
          maxArity = n;
      }
    }
  }
  if (maxArity > MaxCallArgs)
    MaxCallArgs = maxArity;

  // Pre-allocate a shared args buffer for all call instructions.
  if (MaxCallArgs > 0) {
    ir_ctx *ctx = &Ctx;
    SharedCallArgs = ir_ALLOCA(ir_CONST_I32(MaxCallArgs * sizeof(uint64_t)));
  }

  FuncInstrs = Instrs;
  for (uint32_t IX = 0; IX < Instrs.size(); ++IX) {
    CurInstrIdx = IX;
    auto Res = visitInstruction(Instrs[IX]);
    if (!Res) {
      return Unexpect(Res);
    }
  }
  return {};
}

ir_ref WasmToIRBuilder::pop() noexcept {
  if (ValueStack.empty()) {
    return IR_UNUSED;
  }
  ir_ref Val = ValueStack.back();
  ValueStack.pop_back();
  return Val;
}

ir_ref WasmToIRBuilder::ensureValidRef(ir_ref Ref, ir_type Type) noexcept {
  ir_ctx *ctx = &Ctx;
  // Check if ref is valid:
  // - Negative refs are constants (always valid)
  // - Positive refs must be within insns_count
  // - IR_UNUSED (0) is not valid (needs replacement with zero constant)
  if (Ref < 0) {
    return Ref;  // Negative = constant, always valid
  }
  if (Ref > 0 && Ref < static_cast<ir_ref>(Ctx.insns_count)) {
    return Ref;  // Positive and within bounds
  }
  // Invalid ref (0 or out of bounds) - emit a type-appropriate zero constant
  switch (Type) {
  case IR_I32:
    return ir_CONST_I32(0);
  case IR_I64:
    return ir_CONST_I64(0);
  case IR_FLOAT:
    return ir_CONST_FLOAT(0.0f);
  case IR_DOUBLE:
    return ir_CONST_DOUBLE(0.0);
  case IR_ADDR:
    return ir_CONST_ADDR(0);
  case IR_U32:
    return ir_CONST_U32(0);
  default:
    return ir_CONST_I32(0);
  }
}

ir_ref WasmToIRBuilder::coerceToType(ir_ref Value, ir_type TargetType) noexcept {
  ir_ctx *ctx = &Ctx;
  
  // If value is IR_UNUSED (0), return a zero constant of target type.
  // Note: Negative ir_refs are VALID - they represent constants in the IR.
  if (Value == IR_UNUSED) {
    return ensureValidRef(IR_UNUSED, TargetType);  // Will emit zero constant
  }
  
  // Validate the reference is within bounds
  // Negative refs are constants (valid), positive refs must be < insns_count
  if (Value > 0 && Value >= static_cast<ir_ref>(Ctx.insns_count)) {
    return ensureValidRef(IR_UNUSED, TargetType);  // Invalid ref, use zero
  }
  
  ir_type SrcType = static_cast<ir_type>(Ctx.ir_base[Value].type);
  
  // Same type - no coercion needed
  if (SrcType == TargetType) {
    return Value;
  }
  
  // Handle signed/unsigned variants as compatible
  if ((SrcType == IR_I32 && TargetType == IR_U32) ||
      (SrcType == IR_U32 && TargetType == IR_I32)) {
    // Just use the value - they're the same bit pattern
    return Value;
  }
  if ((SrcType == IR_I64 && TargetType == IR_U64) ||
      (SrcType == IR_U64 && TargetType == IR_I64)) {
    return Value;
  }
  
  // Type conversion needed
  switch (TargetType) {
  case IR_I32:
  case IR_U32:
    if (SrcType == IR_I64 || SrcType == IR_U64) {
      return ir_TRUNC_I32(Value);
    } else if (SrcType == IR_FLOAT) {
      return ir_FP2I32(Value);
    } else if (SrcType == IR_DOUBLE) {
      return ir_FP2I32(Value);
    }
    break;
  case IR_I64:
    if (SrcType == IR_I32) {
      return ir_SEXT_I64(Value);
    } else if (SrcType == IR_U32) {
      return ir_ZEXT_I64(Value);
    } else if (SrcType == IR_FLOAT || SrcType == IR_DOUBLE) {
      return ir_FP2I64(Value);
    }
    break;
  case IR_U64:
    if (SrcType == IR_I32 || SrcType == IR_U32) {
      return ir_ZEXT_U64(Value);
    }
    break;
  case IR_FLOAT:
    if (SrcType == IR_I32 || SrcType == IR_I64 || SrcType == IR_U32 || SrcType == IR_U64) {
      return ir_INT2F(Value);
    } else if (SrcType == IR_DOUBLE) {
      return ir_D2F(Value);
    }
    break;
  case IR_DOUBLE:
    if (SrcType == IR_I32 || SrcType == IR_I64 || SrcType == IR_U32 || SrcType == IR_U64) {
      return ir_INT2D(Value);
    } else if (SrcType == IR_FLOAT) {
      return ir_F2D(Value);
    }
    break;
  default:
    break;
  }
  
  // Can't coerce - return a zero constant of target type
  return ensureValidRef(IR_UNUSED, TargetType);
}

ir_ref WasmToIRBuilder::peek(uint32_t Depth) const noexcept {
  if (Depth >= ValueStack.size()) {
    return IR_UNUSED;
  }
  return ValueStack[ValueStack.size() - 1 - Depth];
}

ir_type WasmToIRBuilder::wasmTypeToIRType(ValType Type) const noexcept {
  // Get type code from ValType
  auto Code = Type.getCode();
  if (Code == TypeCode::I32) {
    return IR_I32;
  } else if (Code == TypeCode::I64) {
    return IR_I64;
  } else if (Code == TypeCode::F32) {
    return IR_FLOAT;
  } else if (Code == TypeCode::F64) {
    return IR_DOUBLE;
  } else {
    return IR_ADDR; // Default to address type
  }
}

ir_ref WasmToIRBuilder::getOrEmitReturnValue() noexcept {
  ir_ctx *ctx = &Ctx;
  if (Ctx.ret_type == static_cast<ir_type>(-1) || Ctx.ret_type == IR_VOID) {
    return IR_UNUSED;
  }
  if (!ValueStack.empty()) {
    ir_ref val = pop();
    // IR backend asserts all returns have the same type; only pass if it matches.
    if (val != IR_UNUSED && val < static_cast<ir_ref>(Ctx.insns_count) &&
        Ctx.ir_base[val].type == Ctx.ret_type) {
      return val;
    }
    // Wrong type or invalid ref: fall through to emit constant of ret_type
  }
  // Emit constant so ir_RETURN type matches Ctx.ret_type (empty stack or wrong type).
  switch (Ctx.ret_type) {
  case IR_I32:
    return ir_CONST_I32(0);
  case IR_I64:
    return ir_CONST_I64(0);
  case IR_FLOAT:
    return ir_CONST_FLOAT(0.0f);
  case IR_DOUBLE:
    return ir_CONST_DOUBLE(0.0);
  case IR_ADDR:
    return ir_CONST_ADDR(0);
  default:
    return IR_UNUSED;
  }
}

Expect<void> WasmToIRBuilder::visitInstruction(const AST::Instruction &Instr) {
  OpCode Op = Instr.getOpCode();

  // Skip dead code - when path is terminated, only process control instructions
  // that might restart the path (End, Else, Block, Loop, If)
  if (CurrentPathTerminated) {
    bool isControlRestart = (Op == OpCode::End || Op == OpCode::Else ||
                             Op == OpCode::Block || Op == OpCode::Loop ||
                             Op == OpCode::If);
    if (!isControlRestart) {
      return {};  // Skip dead code
    }
  }

  // Dispatch based on instruction type
  switch (Op) {
  // Constants
  case OpCode::I32__const:
  case OpCode::I64__const:
  case OpCode::F32__const:
  case OpCode::F64__const:
    return visitConst(Instr);

  // Local operations
  case OpCode::Local__get:
  case OpCode::Local__set:
  case OpCode::Local__tee:
    return visitLocal(Instr);

  // Global operations
  case OpCode::Global__get:
  case OpCode::Global__set:
    return visitGlobal(Instr);

  // Binary arithmetic operations - i32
  case OpCode::I32__add:
  case OpCode::I32__sub:
  case OpCode::I32__mul:
  case OpCode::I32__div_s:
  case OpCode::I32__div_u:
  case OpCode::I32__rem_s:
  case OpCode::I32__rem_u:
  case OpCode::I32__and:
  case OpCode::I32__or:
  case OpCode::I32__xor:
  case OpCode::I32__shl:
  case OpCode::I32__shr_s:
  case OpCode::I32__shr_u:
  case OpCode::I32__rotl:
  case OpCode::I32__rotr:
  // Binary arithmetic operations - i64
  case OpCode::I64__add:
  case OpCode::I64__sub:
  case OpCode::I64__mul:
  case OpCode::I64__div_s:
  case OpCode::I64__div_u:
  case OpCode::I64__rem_s:
  case OpCode::I64__rem_u:
  case OpCode::I64__and:
  case OpCode::I64__or:
  case OpCode::I64__xor:
  case OpCode::I64__shl:
  case OpCode::I64__shr_s:
  case OpCode::I64__shr_u:
  case OpCode::I64__rotl:
  case OpCode::I64__rotr:
  // Binary operations - f32/f64
  case OpCode::F32__add:
  case OpCode::F32__sub:
  case OpCode::F32__mul:
  case OpCode::F32__div:
  case OpCode::F32__min:
  case OpCode::F32__max:
  case OpCode::F32__copysign:
  case OpCode::F64__add:
  case OpCode::F64__sub:
  case OpCode::F64__mul:
  case OpCode::F64__div:
  case OpCode::F64__min:
  case OpCode::F64__max:
  case OpCode::F64__copysign:
    return visitBinary(Op);

  // Comparison operations
  case OpCode::I32__eq:
  case OpCode::I32__ne:
  case OpCode::I32__lt_s:
  case OpCode::I32__lt_u:
  case OpCode::I32__le_s:
  case OpCode::I32__le_u:
  case OpCode::I32__gt_s:
  case OpCode::I32__gt_u:
  case OpCode::I32__ge_s:
  case OpCode::I32__ge_u:
  case OpCode::I64__eq:
  case OpCode::I64__ne:
  case OpCode::I64__lt_s:
  case OpCode::I64__lt_u:
  case OpCode::I64__le_s:
  case OpCode::I64__le_u:
  case OpCode::I64__gt_s:
  case OpCode::I64__gt_u:
  case OpCode::I64__ge_s:
  case OpCode::I64__ge_u:
  case OpCode::F32__eq:
  case OpCode::F32__ne:
  case OpCode::F32__lt:
  case OpCode::F32__le:
  case OpCode::F32__gt:
  case OpCode::F32__ge:
  case OpCode::F64__eq:
  case OpCode::F64__ne:
  case OpCode::F64__lt:
  case OpCode::F64__le:
  case OpCode::F64__gt:
  case OpCode::F64__ge:
    return visitCompare(Op);

  // Unary operations
  case OpCode::I32__eqz:
  case OpCode::I32__clz:
  case OpCode::I32__ctz:
  case OpCode::I32__popcnt:
  case OpCode::I64__eqz:
  case OpCode::I64__clz:
  case OpCode::I64__ctz:
  case OpCode::I64__popcnt:
  case OpCode::F32__abs:
  case OpCode::F32__neg:
  case OpCode::F32__sqrt:
  case OpCode::F32__ceil:
  case OpCode::F32__floor:
  case OpCode::F32__trunc:
  case OpCode::F32__nearest:
  case OpCode::F64__abs:
  case OpCode::F64__neg:
  case OpCode::F64__sqrt:
  case OpCode::F64__ceil:
  case OpCode::F64__floor:
  case OpCode::F64__trunc:
  case OpCode::F64__nearest:
    return visitUnary(Op);

  // Parametric operations
  case OpCode::Drop:
  case OpCode::Select:
  case OpCode::Select_t:
    return visitParametric(Instr);

  // Control flow
  case OpCode::Block:
  case OpCode::Loop:
  case OpCode::If:
  case OpCode::Else:
  case OpCode::End:
  case OpCode::Br:
  case OpCode::Br_if:
  case OpCode::Br_table:
  case OpCode::Return:
  case OpCode::Nop:
  case OpCode::Unreachable:
    return visitControl(Instr);

  // Function calls
  case OpCode::Call:
  case OpCode::Call_indirect:
    return visitCall(Instr);

  // Memory operations
  case OpCode::I32__load:
  case OpCode::I64__load:
  case OpCode::F32__load:
  case OpCode::F64__load:
  case OpCode::I32__load8_s:
  case OpCode::I32__load8_u:
  case OpCode::I32__load16_s:
  case OpCode::I32__load16_u:
  case OpCode::I64__load8_s:
  case OpCode::I64__load8_u:
  case OpCode::I64__load16_s:
  case OpCode::I64__load16_u:
  case OpCode::I64__load32_s:
  case OpCode::I64__load32_u:
  case OpCode::I32__store:
  case OpCode::I64__store:
  case OpCode::F32__store:
  case OpCode::F64__store:
  case OpCode::I32__store8:
  case OpCode::I32__store16:
  case OpCode::I64__store8:
  case OpCode::I64__store16:
  case OpCode::I64__store32:
    return visitMemory(Instr);

  // Type conversion operations
  case OpCode::I32__wrap_i64:
  case OpCode::I64__extend_i32_s:
  case OpCode::I64__extend_i32_u:
  case OpCode::I32__trunc_f32_s:
  case OpCode::I32__trunc_f32_u:
  case OpCode::I32__trunc_f64_s:
  case OpCode::I32__trunc_f64_u:
  case OpCode::I64__trunc_f32_s:
  case OpCode::I64__trunc_f32_u:
  case OpCode::I64__trunc_f64_s:
  case OpCode::I64__trunc_f64_u:
  case OpCode::F32__convert_i32_s:
  case OpCode::F32__convert_i32_u:
  case OpCode::F32__convert_i64_s:
  case OpCode::F32__convert_i64_u:
  case OpCode::F64__convert_i32_s:
  case OpCode::F64__convert_i32_u:
  case OpCode::F64__convert_i64_s:
  case OpCode::F64__convert_i64_u:
  case OpCode::F32__demote_f64:
  case OpCode::F64__promote_f32:
  case OpCode::I32__reinterpret_f32:
  case OpCode::I64__reinterpret_f64:
  case OpCode::F32__reinterpret_i32:
  case OpCode::F64__reinterpret_i64:
  case OpCode::I32__extend8_s:
  case OpCode::I32__extend16_s:
  case OpCode::I64__extend8_s:
  case OpCode::I64__extend16_s:
  case OpCode::I64__extend32_s:
  // Saturating truncation operations
  case OpCode::I32__trunc_sat_f32_s:
  case OpCode::I32__trunc_sat_f32_u:
  case OpCode::I32__trunc_sat_f64_s:
  case OpCode::I32__trunc_sat_f64_u:
  case OpCode::I64__trunc_sat_f32_s:
  case OpCode::I64__trunc_sat_f32_u:
  case OpCode::I64__trunc_sat_f64_s:
  case OpCode::I64__trunc_sat_f64_u:
    return visitConversion(Op);

  // Reference types
  case OpCode::Ref__null:
  case OpCode::Ref__is_null:
  case OpCode::Ref__func:
    return visitRefType(Instr);

  // Table operations
  case OpCode::Table__get:
  case OpCode::Table__set:
  case OpCode::Table__size:
  case OpCode::Table__grow:
  case OpCode::Table__fill:
  case OpCode::Table__copy:
  case OpCode::Table__init:
  case OpCode::Elem__drop:
    return visitTable(Instr);

  // Bulk memory operations
  case OpCode::Memory__copy: {
    // memory.copy: len, src, dst (stack order). Traps on OOB; supports multi-memory.
    ir_ctx *ctx = &Ctx;
    ir_ref N = pop();
    ir_ref Src = pop();
    ir_ref Dst = pop();
    ir_ref EnvPtrVal = ensureValidRef(EnvPtr, IR_ADDR);
    uint32_t dstMemIdx = Instr.getTargetIndex();
    uint32_t srcMemIdx = Instr.getSourceIndex();
    uint8_t ProtoParams[6] = {IR_ADDR, IR_U32, IR_U32, IR_U32, IR_U32, IR_U32};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 6, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_memory_copy, Proto);
    ir_CALL_6(IR_VOID, Fn, EnvPtrVal, ir_CONST_I32(static_cast<int32_t>(dstMemIdx)),
              ir_CONST_I32(static_cast<int32_t>(srcMemIdx)),
              coerceToType(Dst, IR_U32), coerceToType(Src, IR_U32),
              coerceToType(N, IR_U32));
    return {};
  }

  case OpCode::Memory__fill: {
    // memory.fill: len, val, off (stack order). Traps on OOB; supports multi-memory.
    ir_ctx *ctx = &Ctx;
    ir_ref N = pop();
    ir_ref Val = pop();
    ir_ref Off = pop();
    ir_ref EnvPtrVal = ensureValidRef(EnvPtr, IR_ADDR);
    uint32_t memIdx = Instr.getTargetIndex();
    uint8_t ProtoParams[5] = {IR_ADDR, IR_U32, IR_U32, IR_U32, IR_U32};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 5, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_memory_fill, Proto);
    ir_CALL_5(IR_VOID, Fn, EnvPtrVal, ir_CONST_I32(static_cast<int32_t>(memIdx)),
              coerceToType(Off, IR_U32), coerceToType(Val, IR_U32),
              coerceToType(N, IR_U32));
    return {};
  }

  case OpCode::Memory__init: {
    // memory.init: len, src (data offset), dst (mem offset). Target = memIdx, Source = dataIdx.
    ir_ctx *ctx = &Ctx;
    ir_ref Len = pop();
    ir_ref Src = pop();
    ir_ref Dst = pop();
    ir_ref EnvPtrVal = ensureValidRef(EnvPtr, IR_ADDR);
    uint32_t memIdx = Instr.getTargetIndex();
    uint32_t dataIdx = Instr.getSourceIndex();
    uint8_t ProtoParams[6] = {IR_ADDR, IR_U32, IR_U32, IR_U32, IR_U32, IR_U32};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 6, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_memory_init, Proto);
    ir_CALL_6(IR_VOID, Fn, EnvPtrVal, ir_CONST_I32(static_cast<int32_t>(memIdx)),
              ir_CONST_I32(static_cast<int32_t>(dataIdx)),
              coerceToType(Dst, IR_U32), coerceToType(Src, IR_U32),
              coerceToType(Len, IR_U32));
    return {};
  }

  case OpCode::Data__drop: {
    ir_ctx *ctx = &Ctx;
    ir_ref EnvPtrVal = ensureValidRef(EnvPtr, IR_ADDR);
    uint32_t dataIdx = Instr.getTargetIndex();
    uint8_t ProtoParams[2] = {IR_ADDR, IR_U32};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 2, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_data_drop, Proto);
    ir_CALL_2(IR_VOID, Fn, EnvPtrVal,
              ir_CONST_I32(static_cast<int32_t>(dataIdx)));
    return {};
  }
  
  case OpCode::Memory__size: {
    // memory.size: call jit_memory_size(env) -> returns page count
    ir_ctx *ctx = &Ctx;
    ir_ref EnvPtrVal = ensureValidRef(EnvPtr, IR_ADDR);
    ir_ref Fn = ensureValidRef(MemorySizeFnPtr, IR_ADDR);
    uint8_t ProtoParams[1] = {IR_ADDR};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_I32, 1, ProtoParams);
    ir_ref TypedFn = ir_emit2(ctx, IR_OPT(IR_PROTO, IR_ADDR), Fn, Proto);
    ir_ref Result = ir_CALL_1(IR_I32, TypedFn, EnvPtrVal);
    push(Result);
    return {};
  }
  
  case OpCode::Memory__grow: {
    // memory.grow: call jit_memory_grow(env, nPages) -> returns old size or -1
    // After grow, reload MemoryBase from env since buffer may relocate.
    ir_ctx *ctx = &Ctx;
    ir_ref NPagesI32 = pop();
    ir_ref NPages = ensureValidRef(NPagesI32, IR_I32);
    // Cast to u32 for the trampoline
    ir_ref NPagesU32 = coerceToType(NPages, IR_U32);
    ir_ref EnvPtrVal = ensureValidRef(EnvPtr, IR_ADDR);
    ir_ref Fn = ensureValidRef(MemoryGrowFnPtr, IR_ADDR);
    uint8_t ProtoParams[2] = {IR_ADDR, IR_U32};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_I32, 2, ProtoParams);
    ir_ref TypedFn = ir_emit2(ctx, IR_OPT(IR_PROTO, IR_ADDR), Fn, Proto);
    ir_ref Result = ir_CALL_2(IR_I32, TypedFn, EnvPtrVal, NPagesU32);
    // Reload MemoryBase from env since grow may have relocated the buffer.
    // Bounds checks load MemorySizeBytes from env each time, so no SSA reload
    // of the size is needed here.
    MemoryBase = ir_LOAD_A(ir_ADD_A(EnvPtrVal,
        ir_CONST_ADDR(offsetof(JitExecEnv, MemoryBase))));
    push(Result);
    return {};
  }

  default:
    // Unsupported instruction - return error
    return Unexpect(ErrCode::Value::RuntimeError);
  }
}

Expect<void> WasmToIRBuilder::visitConst(const AST::Instruction &Instr) {
  OpCode Op = Instr.getOpCode();
  ir_ctx *ctx = &Ctx; // For IR macros
  ir_ref ConstVal = IR_UNUSED;

  ValVariant NumVal = Instr.getNum();
  
  switch (Op) {
  case OpCode::I32__const:
    ConstVal = ir_CONST_I32(NumVal.get<int32_t>());
    break;
  case OpCode::I64__const:
    ConstVal = ir_CONST_I64(NumVal.get<int64_t>());
    break;
  case OpCode::F32__const:
    ConstVal = ir_CONST_FLOAT(NumVal.get<float>());
    break;
  case OpCode::F64__const:
    ConstVal = ir_CONST_DOUBLE(NumVal.get<double>());
    break;
  default:
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  push(ConstVal);
  return {};
}

Expect<void> WasmToIRBuilder::visitLocal(const AST::Instruction &Instr) {
  OpCode Op = Instr.getOpCode();
  uint32_t LocalIdx = Instr.getTargetIndex();

  if (LocalIdx >= LocalCount) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  switch (Op) {
  case OpCode::Local__get: {
    // In SSA form, just push the local value
    push(Locals[LocalIdx]);
    break;
  }
  case OpCode::Local__set: {
    // Update the local to the new SSA value
    ir_ref Value = pop();
    Locals[LocalIdx] = Value;
    break;
  }
  case OpCode::Local__tee: {
    // Tee: set local but keep value on stack
    ir_ref Value = peek(0);
    Locals[LocalIdx] = Value;
    break;
  }
  default:
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  return {};
}

Expect<void> WasmToIRBuilder::visitGlobal(const AST::Instruction &Instr) {
  ir_ctx *ctx = &Ctx;
  OpCode Op = Instr.getOpCode();
  uint32_t GlobalIdx = Instr.getTargetIndex();

  // Check if we have global type information
  if (GlobalIdx >= ModuleGlobalTypes.size()) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  ValType GlobalType = ModuleGlobalTypes[GlobalIdx];
  ir_type IrType = wasmTypeToIRType(GlobalType);

  // GlobalBasePtr is ValVariant** (pointer to array of ValVariant*)
  // Step 1: Calculate address of pointer: GlobalBasePtr + GlobalIdx * sizeof(void*)
  ir_ref PtrOffset = ir_MUL_A(ir_CONST_ADDR(static_cast<uintptr_t>(GlobalIdx)),
                              ir_CONST_ADDR(sizeof(void*)));
  ir_ref PtrAddr = ir_ADD_A(GlobalBasePtr, PtrOffset);
  
  // Step 2: Load the ValVariant* pointer
  ir_ref GlobalPtr = ir_LOAD_A(PtrAddr);
  
  // Step 3: The GlobalPtr now points to the ValVariant, which has the value at offset 0
  // (ValVariant stores value in first bytes)
  ir_ref GlobalAddr = GlobalPtr;

  switch (Op) {
  case OpCode::Global__get: {
    // Load the global value
    ir_ref Value;
    switch (IrType) {
    case IR_I32:
      Value = ir_LOAD_I32(GlobalAddr);
      break;
    case IR_I64:
      Value = ir_LOAD_I64(GlobalAddr);
      break;
    case IR_FLOAT:
      Value = ir_LOAD_F(GlobalAddr);
      break;
    case IR_DOUBLE:
      Value = ir_LOAD_D(GlobalAddr);
      break;
    default:
      return Unexpect(ErrCode::Value::RuntimeError);
    }
    push(Value);
    break;
  }
  case OpCode::Global__set: {
    // Store the value to the global
    // ir_STORE infers the type from the value
    ir_ref Value = pop();
    ir_STORE(GlobalAddr, Value);
    break;
  }
  default:
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  return {};
}

Expect<void> WasmToIRBuilder::visitBinary(OpCode Op) {
  ir_ctx *ctx = &Ctx; // For IR macros
  ir_ref Right = pop();
  ir_ref Left = pop();
  ir_ref Result = IR_UNUSED;

  switch (Op) {
  // I32 arithmetic
  case OpCode::I32__add:
    Result = ir_ADD_I32(Left, Right);
    break;
  case OpCode::I32__sub:
    Result = ir_SUB_I32(Left, Right);
    break;
  case OpCode::I32__mul:
    Result = ir_MUL_I32(Left, Right);
    break;
  case OpCode::I32__div_s:
    Result = ir_DIV_I32(Left, Right);
    break;
  case OpCode::I32__div_u: {
    // IR DIV_U32 requires U32 operands; stack has I32. Call helper (same bits).
    ir_ref Proto = ir_proto_2(ctx, IR_FASTCALL_FUNC, IR_I32, IR_I32, IR_I32);
    ir_ref Func = ir_const_func_addr(ctx, (uintptr_t)&wasm_i32_div_u, Proto);
    Result = ir_CALL_2(IR_I32, Func, Left, Right);
    break;
  }
  case OpCode::I32__rem_s:
    Result = ir_MOD_I32(Left, Right);
    break;
  case OpCode::I32__rem_u: {
    // IR MOD_U32 requires U32 operands; stack has I32. Call helper (same bits).
    ir_ref Proto = ir_proto_2(ctx, IR_FASTCALL_FUNC, IR_I32, IR_I32, IR_I32);
    ir_ref Func = ir_const_func_addr(ctx, (uintptr_t)&wasm_i32_rem_u, Proto);
    Result = ir_CALL_2(IR_I32, Func, Left, Right);
    break;
  }
  case OpCode::I32__and:
    Result = ir_AND_I32(Left, Right);
    break;
  case OpCode::I32__or:
    Result = ir_OR_I32(Left, Right);
    break;
  case OpCode::I32__xor:
    Result = ir_XOR_I32(Left, Right);
    break;
  case OpCode::I32__shl:
    Result = ir_SHL_I32(Left, Right);
    break;
  case OpCode::I32__shr_s:
    Result = ir_SAR_I32(Left, Right);
    break;
  case OpCode::I32__shr_u:
    Result = ir_SHR_I32(Left, Right);
    break;
  case OpCode::I32__rotl:
    Result = ir_ROL_I32(Left, Right);
    break;
  case OpCode::I32__rotr:
    Result = ir_ROR_I32(Left, Right);
    break;

  // I64 arithmetic
  case OpCode::I64__add:
    Result = ir_ADD_I64(Left, Right);
    break;
  case OpCode::I64__sub:
    Result = ir_SUB_I64(Left, Right);
    break;
  case OpCode::I64__mul:
    Result = ir_MUL_I64(Left, Right);
    break;
  case OpCode::I64__div_s:
    Result = ir_DIV_I64(Left, Right);
    break;
  case OpCode::I64__div_u: {
    ir_ref Proto = ir_proto_2(ctx, IR_FASTCALL_FUNC, IR_I64, IR_I64, IR_I64);
    ir_ref Func = ir_const_func_addr(ctx, (uintptr_t)&wasm_i64_div_u, Proto);
    Result = ir_CALL_2(IR_I64, Func, Left, Right);
    break;
  }
  case OpCode::I64__rem_s:
    Result = ir_MOD_I64(Left, Right);
    break;
  case OpCode::I64__rem_u: {
    ir_ref Proto = ir_proto_2(ctx, IR_FASTCALL_FUNC, IR_I64, IR_I64, IR_I64);
    ir_ref Func = ir_const_func_addr(ctx, (uintptr_t)&wasm_i64_rem_u, Proto);
    Result = ir_CALL_2(IR_I64, Func, Left, Right);
    break;
  }
  case OpCode::I64__and:
    Result = ir_AND_I64(Left, Right);
    break;
  case OpCode::I64__or:
    Result = ir_OR_I64(Left, Right);
    break;
  case OpCode::I64__xor:
    Result = ir_XOR_I64(Left, Right);
    break;
  case OpCode::I64__shl:
    Result = ir_SHL_I64(Left, Right);
    break;
  case OpCode::I64__shr_s:
    Result = ir_SAR_I64(Left, Right);
    break;
  case OpCode::I64__shr_u:
    Result = ir_SHR_I64(Left, Right);
    break;
  case OpCode::I64__rotl:
    Result = ir_ROL_I64(Left, Right);
    break;
  case OpCode::I64__rotr:
    Result = ir_ROR_I64(Left, Right);
    break;

  // F32 arithmetic
  case OpCode::F32__add:
    Result = ir_ADD_F(Left, Right);
    break;
  case OpCode::F32__sub:
    Result = ir_SUB_F(Left, Right);
    break;
  case OpCode::F32__mul:
    Result = ir_MUL_F(Left, Right);
    break;
  case OpCode::F32__div:
    Result = ir_DIV_F(Left, Right);
    break;
  case OpCode::F32__min:
    Result = ir_MIN_F(Left, Right);
    break;
  case OpCode::F32__max:
    Result = ir_MAX_F(Left, Right);
    break;
  case OpCode::F32__copysign: {
    ir_ref Proto = ir_proto_2(ctx, IR_FASTCALL_FUNC, IR_FLOAT, IR_FLOAT, IR_FLOAT);
    ir_ref Func = ir_const_func_addr(ctx, (uintptr_t)&wasm_f32_copysign, Proto);
    Result = ir_CALL_2(IR_FLOAT, Func, Left, Right);
    break;
  }

  // F64 arithmetic
  case OpCode::F64__add:
    Result = ir_ADD_D(Left, Right);
    break;
  case OpCode::F64__sub:
    Result = ir_SUB_D(Left, Right);
    break;
  case OpCode::F64__mul:
    Result = ir_MUL_D(Left, Right);
    break;
  case OpCode::F64__div:
    Result = ir_DIV_D(Left, Right);
    break;
  case OpCode::F64__min:
    Result = ir_MIN_D(Left, Right);
    break;
  case OpCode::F64__max:
    Result = ir_MAX_D(Left, Right);
    break;
  case OpCode::F64__copysign: {
    ir_ref Proto = ir_proto_2(ctx, IR_FASTCALL_FUNC, IR_DOUBLE, IR_DOUBLE, IR_DOUBLE);
    ir_ref Func = ir_const_func_addr(ctx, (uintptr_t)&wasm_f64_copysign, Proto);
    Result = ir_CALL_2(IR_DOUBLE, Func, Left, Right);
    break;
  }

  default:
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  push(Result);
  return {};
}

Expect<void> WasmToIRBuilder::visitCompare(OpCode Op) {
  ir_ctx *ctx = &Ctx; // For IR macros
  ir_ref Right = pop();
  ir_ref Left = pop();
  ir_ref Result = IR_UNUSED;

  switch (Op) {
  // I32/I64 comparisons - IR uses generic comparison macros
  case OpCode::I32__eq:
  case OpCode::I64__eq:
    Result = ir_EQ(Left, Right);
    break;
  case OpCode::I32__ne:
  case OpCode::I64__ne:
    Result = ir_NE(Left, Right);
    break;
  case OpCode::I32__lt_s:
  case OpCode::I64__lt_s:
    Result = ir_LT(Left, Right);
    break;
  case OpCode::I32__lt_u:
  case OpCode::I64__lt_u:
    Result = ir_ULT(Left, Right);
    break;
  case OpCode::I32__le_s:
  case OpCode::I64__le_s:
    Result = ir_LE(Left, Right);
    break;
  case OpCode::I32__le_u:
  case OpCode::I64__le_u:
    Result = ir_ULE(Left, Right);
    break;
  case OpCode::I32__gt_s:
  case OpCode::I64__gt_s:
    Result = ir_GT(Left, Right);
    break;
  case OpCode::I32__gt_u:
  case OpCode::I64__gt_u:
    Result = ir_UGT(Left, Right);
    break;
  case OpCode::I32__ge_s:
  case OpCode::I64__ge_s:
    Result = ir_GE(Left, Right);
    break;
  case OpCode::I32__ge_u:
  case OpCode::I64__ge_u:
    Result = ir_UGE(Left, Right);
    break;

  // F32/F64 comparisons - also use generic macros
  case OpCode::F32__eq:
  case OpCode::F64__eq:
    Result = ir_EQ(Left, Right);
    break;
  case OpCode::F32__ne:
  case OpCode::F64__ne:
    Result = ir_NE(Left, Right);
    break;
  case OpCode::F32__lt:
  case OpCode::F64__lt:
    Result = ir_LT(Left, Right);
    break;
  case OpCode::F32__le:
  case OpCode::F64__le:
    Result = ir_LE(Left, Right);
    break;
  case OpCode::F32__gt:
  case OpCode::F64__gt:
    Result = ir_GT(Left, Right);
    break;
  case OpCode::F32__ge:
  case OpCode::F64__ge:
    Result = ir_GE(Left, Right);
    break;

  default:
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // WebAssembly comparisons return i32 0/1. The IR backend produces BOOL from
  // ir_EQ/ir_NE/ir_LT/etc. Convert BOOL to I32 (0 or 1) so ir_check passes and
  // later uses (AND, PHI, etc.) see the correct type.
  switch (Op) {
  case OpCode::I32__eq:
  case OpCode::I32__ne:
  case OpCode::I32__lt_s:
  case OpCode::I32__lt_u:
  case OpCode::I32__le_s:
  case OpCode::I32__le_u:
  case OpCode::I32__gt_s:
  case OpCode::I32__gt_u:
  case OpCode::I32__ge_s:
  case OpCode::I32__ge_u:
  case OpCode::I64__eq:
  case OpCode::I64__ne:
  case OpCode::I64__lt_s:
  case OpCode::I64__lt_u:
  case OpCode::I64__le_s:
  case OpCode::I64__le_u:
  case OpCode::I64__gt_s:
  case OpCode::I64__gt_u:
  case OpCode::I64__ge_s:
  case OpCode::I64__ge_u:
  case OpCode::F32__eq:
  case OpCode::F32__ne:
  case OpCode::F32__lt:
  case OpCode::F32__le:
  case OpCode::F32__gt:
  case OpCode::F32__ge:
  case OpCode::F64__eq:
  case OpCode::F64__ne:
  case OpCode::F64__lt:
  case OpCode::F64__le:
  case OpCode::F64__gt:
  case OpCode::F64__ge: {
    ir_ref One = ir_CONST_I32(1);
    ir_ref Zero = ir_CONST_I32(0);
    Result = ir_COND(IR_I32, Result, One, Zero);
    break;
  }
  default:
    break;
  }

  push(Result);
  return {};
}

Expect<void> WasmToIRBuilder::visitUnary(OpCode Op) {
  ir_ctx *ctx = &Ctx; // For IR macros
  ir_ref Operand = pop();
  ir_ref Result = IR_UNUSED;

  switch (Op) {
  // I32 unary
  case OpCode::I32__eqz:
    // eqz returns i32 0/1. ir_EQ produces BOOL; convert to I32 for ir_check.
    {
      ir_ref Cmp = ir_EQ(Operand, ir_CONST_I32(0));
      ir_ref One = ir_CONST_I32(1);
      ir_ref Zero = ir_CONST_I32(0);
      Result = ir_COND(IR_I32, Cmp, One, Zero);
    }
    break;
  case OpCode::I32__clz:
    // WebAssembly defines clz(0) = 32 for i32.
    // Implement: Operand == 0 ? 32 : ctlz(Operand)
    {
      ir_ref IsZero = ir_EQ(Operand, ir_CONST_I32(0));
      ir_ref ClzVal = ir_CTLZ_I32(Operand);
      ir_ref BitWidth = ir_CONST_I32(32);
      Result = ir_COND(IR_I32, IsZero, BitWidth, ClzVal);
    }
    break;
  case OpCode::I32__ctz:
    // WebAssembly defines ctz(0) = 32 for i32.
    // Implement: Operand == 0 ? 32 : cttz(Operand)
    {
      ir_ref IsZero = ir_EQ(Operand, ir_CONST_I32(0));
      ir_ref CtzVal = ir_CTTZ_I32(Operand);
      ir_ref BitWidth = ir_CONST_I32(32);
      Result = ir_COND(IR_I32, IsZero, BitWidth, CtzVal);
    }
    break;
  case OpCode::I32__popcnt: {
    // Lower to software popcnt (parallel bit count) to avoid x86 backend bug where
    // ir_emit_bit_count does not load the operand when op1_reg is NONE.
    ir_ref x = Operand;
    x = ir_SUB_I32(x, ir_AND_I32(ir_SHR_I32(x, ir_CONST_I32(1)),
                                  ir_CONST_I32(0x55555555)));
    x = ir_ADD_I32(ir_AND_I32(x, ir_CONST_I32(0x33333333)),
                   ir_AND_I32(ir_SHR_I32(x, ir_CONST_I32(2)),
                              ir_CONST_I32(0x33333333)));
    x = ir_AND_I32(ir_ADD_I32(x, ir_SHR_I32(x, ir_CONST_I32(4))),
                   ir_CONST_I32(0x0F0F0F0F));
    Result = ir_SHR_I32(ir_MUL_I32(x, ir_CONST_I32(0x01010101)),
                       ir_CONST_I32(24));
    break;
  }

  // I64 unary
  case OpCode::I64__eqz:
    // eqz(i64) returns i32 0/1. ir_EQ produces BOOL; convert to I32 for ir_check.
    {
      ir_ref Cmp = ir_EQ(Operand, ir_CONST_I64(0));
      ir_ref One = ir_CONST_I32(1);
      ir_ref Zero = ir_CONST_I32(0);
      Result = ir_COND(IR_I32, Cmp, One, Zero);
    }
    break;
  case OpCode::I64__clz:
    // WebAssembly defines clz(0) = 64 for i64.
    {
      ir_ref IsZero = ir_EQ(Operand, ir_CONST_I64(0));
      ir_ref ClzVal = ir_CTLZ_I64(Operand);
      ir_ref BitWidth = ir_CONST_I64(64);
      Result = ir_COND(IR_I64, IsZero, BitWidth, ClzVal);
    }
    break;
  case OpCode::I64__ctz:
    // WebAssembly defines ctz(0) = 64 for i64.
    {
      ir_ref IsZero = ir_EQ(Operand, ir_CONST_I64(0));
      ir_ref CtzVal = ir_CTTZ_I64(Operand);
      ir_ref BitWidth = ir_CONST_I64(64);
      Result = ir_COND(IR_I64, IsZero, BitWidth, CtzVal);
    }
    break;
  case OpCode::I64__popcnt: {
    // Lower to software popcnt to avoid x86 backend bug (see I32__popcnt).
    ir_ref x = Operand;
    const int64_t m1 = 0x5555555555555555LL;
    const int64_t m2 = 0x3333333333333333LL;
    const int64_t m4 = 0x0F0F0F0F0F0F0F0FLL;
    const int64_t m8 = 0x0101010101010101LL;
    x = ir_SUB_I64(x, ir_AND_I64(ir_SHR_I64(x, ir_CONST_I64(1)),
                                  ir_CONST_I64(m1)));
    x = ir_ADD_I64(ir_AND_I64(x, ir_CONST_I64(m2)),
                   ir_AND_I64(ir_SHR_I64(x, ir_CONST_I64(2)),
                              ir_CONST_I64(m2)));
    x = ir_AND_I64(ir_ADD_I64(x, ir_SHR_I64(x, ir_CONST_I64(4))),
                   ir_CONST_I64(m4));
    Result = ir_SHR_I64(ir_MUL_I64(x, ir_CONST_I64(m8)),
                       ir_CONST_I64(56));
    break;
  }

  // F32 unary
  case OpCode::F32__abs:
    Result = ir_ABS_F(Operand);
    break;
  case OpCode::F32__neg:
    Result = ir_NEG_F(Operand);
    break;
  // TODO: SQRT, CEIL, FLOOR, TRUNC, NEAREST require intrinsic calls
  // For now, return the operand unchanged (placeholder)
  case OpCode::F32__sqrt:
  case OpCode::F32__ceil:
  case OpCode::F32__floor:
  case OpCode::F32__trunc:
  case OpCode::F32__nearest:
    // Placeholder: would need to call C library functions
    Result = Operand;
    break;

  // F64 unary
  case OpCode::F64__abs:
    Result = ir_ABS_D(Operand);
    break;
  case OpCode::F64__neg:
    Result = ir_NEG_D(Operand);
    break;
  // TODO: SQRT, CEIL, FLOOR, TRUNC, NEAREST require intrinsic calls
  case OpCode::F64__sqrt:
  case OpCode::F64__ceil:
  case OpCode::F64__floor:
  case OpCode::F64__trunc:
  case OpCode::F64__nearest:
    // Placeholder: would need to call C library functions
    Result = Operand;
    break;

  default:
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  push(Result);
  return {};
}

Expect<void> WasmToIRBuilder::visitConversion(OpCode Op) {
  ir_ctx *ctx = &Ctx; // For IR macros
  ir_ref Operand = pop();
  ir_ref Result = IR_UNUSED;
  
  // Helper to get the IR type of an operand
  auto getOperandType = [this](ir_ref Ref) -> ir_type {
    if (Ref > 0 && Ref < static_cast<ir_ref>(Ctx.insns_count)) {
      return static_cast<ir_type>(Ctx.ir_base[Ref].type);
    }
    return IR_I32;  // Default fallback
  };

  switch (Op) {
  // Integer wrap/extend
  case OpCode::I32__wrap_i64: {
    // Truncate i64 to i32
    ir_type SrcType = getOperandType(Operand);
    if (SrcType == IR_I32 || SrcType == IR_U32) {
      // Already i32, no truncation needed
      Result = Operand;
    } else {
      Result = ir_TRUNC_I32(Operand);
    }
    break;
  }
  case OpCode::I64__extend_i32_s:
    // Sign-extend i32 to i64
    Result = ir_SEXT_I64(Operand);
    break;
  case OpCode::I64__extend_i32_u:
    // Zero-extend i32 to i64 (use I64 type for consistency with other i64 ops)
    Result = ir_ZEXT_I64(Operand);
    break;

  // Float to integer truncation (non-saturating)
  case OpCode::I32__trunc_f32_s:
  case OpCode::I32__trunc_f64_s:
    Result = ir_FP2I32(Operand);
    break;
  case OpCode::I32__trunc_f32_u:
  case OpCode::I32__trunc_f64_u:
    // Use I32 for WebAssembly stack consistency (FP2U32 produces U32 which causes type mismatches)
    Result = ir_FP2I32(Operand);
    break;
  case OpCode::I64__trunc_f32_s:
  case OpCode::I64__trunc_f64_s:
    Result = ir_FP2I64(Operand);
    break;
  case OpCode::I64__trunc_f32_u:
  case OpCode::I64__trunc_f64_u:
    // Use I64 for WebAssembly stack consistency (FP2U64 produces U64 which causes type mismatches)
    Result = ir_FP2I64(Operand);
    break;

  // Saturating truncation (same as non-saturating for now)
  // Note: Proper saturation would require additional bounds checking
  case OpCode::I32__trunc_sat_f32_s:
  case OpCode::I32__trunc_sat_f64_s:
    Result = ir_FP2I32(Operand);
    break;
  case OpCode::I32__trunc_sat_f32_u:
  case OpCode::I32__trunc_sat_f64_u:
    // Use I32 for consistency
    Result = ir_FP2I32(Operand);
    break;
  case OpCode::I64__trunc_sat_f32_s:
  case OpCode::I64__trunc_sat_f64_s:
    Result = ir_FP2I64(Operand);
    break;
  case OpCode::I64__trunc_sat_f32_u:
  case OpCode::I64__trunc_sat_f64_u:
    // Use I64 for consistency
    Result = ir_FP2I64(Operand);
    break;

  // Integer to float conversion
  case OpCode::F32__convert_i32_s:
  case OpCode::F32__convert_i64_s:
    Result = ir_INT2F(Operand);
    break;
  case OpCode::F32__convert_i32_u:
    // Zero-extend to i64 first, then convert (for unsigned semantics)
    Result = ir_INT2F(ir_ZEXT_I64(Operand));
    break;
  case OpCode::F32__convert_i64_u: {
    // For unsigned i64 to f32, need special handling for large values
    // Simplified: just use INT2F (may lose precision for large unsigned values)
    Result = ir_INT2F(Operand);
    break;
  }
  case OpCode::F64__convert_i32_s:
  case OpCode::F64__convert_i64_s:
    Result = ir_INT2D(Operand);
    break;
  case OpCode::F64__convert_i32_u:
    // Zero-extend to i64 first, then convert
    Result = ir_INT2D(ir_ZEXT_I64(Operand));
    break;
  case OpCode::F64__convert_i64_u: {
    // For unsigned i64 to f64, need special handling for large values
    // Simplified: just use INT2D (may lose precision for large unsigned values)
    Result = ir_INT2D(Operand);
    break;
  }

  // Float promotion/demotion
  case OpCode::F32__demote_f64:
    Result = ir_D2F(Operand);
    break;
  case OpCode::F64__promote_f32:
    Result = ir_F2D(Operand);
    break;

  // Reinterpret (bitcast)
  case OpCode::I32__reinterpret_f32:
    Result = ir_BITCAST_I32(Operand);
    break;
  case OpCode::I64__reinterpret_f64:
    Result = ir_BITCAST_I64(Operand);
    break;
  case OpCode::F32__reinterpret_i32:
    Result = ir_BITCAST_F(Operand);
    break;
  case OpCode::F64__reinterpret_i64:
    Result = ir_BITCAST_D(Operand);
    break;

  // Sign extension from partial width
  case OpCode::I32__extend8_s: {
    // Sign-extend lowest 8 bits to i32
    // Shift left 24, then arithmetic shift right 24
    ir_ref Shifted = ir_SHL_I32(Operand, ir_CONST_I32(24));
    Result = ir_SAR_I32(Shifted, ir_CONST_I32(24));
    break;
  }
  case OpCode::I32__extend16_s: {
    // Sign-extend lowest 16 bits to i32
    ir_ref Shifted = ir_SHL_I32(Operand, ir_CONST_I32(16));
    Result = ir_SAR_I32(Shifted, ir_CONST_I32(16));
    break;
  }
  case OpCode::I64__extend8_s: {
    // Sign-extend lowest 8 bits to i64
    ir_ref Shifted = ir_SHL_I64(Operand, ir_CONST_I64(56));
    Result = ir_SAR_I64(Shifted, ir_CONST_I64(56));
    break;
  }
  case OpCode::I64__extend16_s: {
    // Sign-extend lowest 16 bits to i64
    ir_ref Shifted = ir_SHL_I64(Operand, ir_CONST_I64(48));
    Result = ir_SAR_I64(Shifted, ir_CONST_I64(48));
    break;
  }
  case OpCode::I64__extend32_s: {
    // Sign-extend lowest 32 bits to i64
    ir_ref Shifted = ir_SHL_I64(Operand, ir_CONST_I64(32));
    Result = ir_SAR_I64(Shifted, ir_CONST_I64(32));
    break;
  }

  default:
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  push(Result);
  return {};
}

Expect<void> WasmToIRBuilder::visitParametric(const AST::Instruction &Instr) {
  ir_ctx *ctx = &Ctx; // For IR macros
  OpCode Op = Instr.getOpCode();

  switch (Op) {
  case OpCode::Drop: {
    // Just pop from stack, value is discarded
    pop();
    break;
  }
  case OpCode::Select:
  case OpCode::Select_t: {
    // Select takes 3 operands: val1, val2, cond
    // Returns val1 if cond != 0, else val2
    // Stack order: [... val1 val2 cond]
    ir_ref Cond = pop();
    ir_ref Val2 = pop();
    ir_ref Val1 = pop();
    
    // Infer result type from Val1 (in WebAssembly, Val1 and Val2 have same type)
    // Note: negative refs are constants, positive refs are instructions
    ir_type ResultType = IR_I32;  // default
    if (Val1 != IR_UNUSED) {
      if (Val1 > 0 && Val1 < static_cast<ir_ref>(Ctx.insns_count)) {
        ResultType = static_cast<ir_type>(Ctx.ir_base[Val1].type);
      } else if (Val1 < 0) {
        ResultType = static_cast<ir_type>(Ctx.ir_base[Val1].type);
      }
    }
    
    // If condition is a constant, evaluate at compile time
    // IR backend crashes with COND having constant condition
    ir_ref Result;
    if (Cond < 0) {
      // Constant condition - evaluate now
      ir_insn *condInsn = &Ctx.ir_base[Cond];
      int64_t condVal = condInsn->val.i64;
      Result = (condVal != 0) ? Val1 : Val2;
    } else {
      // Use IR's COND operation: COND(condition, true_val, false_val)
      Result = ir_COND(ResultType, Cond, Val1, Val2);
    }
    push(Result);
    break;
  }
  default:
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  return {};
}

ir_ref WasmToIRBuilder::buildMemoryAddress(ir_ref Base, uint32_t Offset) {
  ir_ctx *ctx = &Ctx; // For IR macros
  
  // Compute Wasm effective address in I32 (base from stack is Wasm i32).
  // Use I32 ops so ir_check does not see I32 vs U32 mismatch.
  ir_ref WasmAddr = Base;
  if (Offset != 0) {
    WasmAddr = ir_ADD_I32(Base, ir_CONST_I32(static_cast<int32_t>(Offset)));
  }
  
  // Zero-extend Wasm address (32-bit) to native address width
  // Then add to memory base pointer
  ir_ref WasmAddrExt = ir_ZEXT_A(WasmAddr);
  ir_ref EffectiveAddr = ir_ADD_A(MemoryBase, WasmAddrExt);
  
  return EffectiveAddr;
}

void WasmToIRBuilder::buildBoundsCheck(ir_ref Base, uint32_t Offset,
                                       uint32_t AccessSize) {
  if (!irJitBoundCheckEnabled())
    return;

  ir_ctx *ctx = &Ctx;

  // Effective address ea must match buildMemoryAddress: i32 base + offset with
  // 32-bit wrap, then end = zext_u64(ea) + access_size (Wasm spec).
  // Trap if end > MemorySizeBytes (jit_bounds_check loads size from env — correct
  // after memory.grow).
  //
  // Use a single outlined call instead of IF + LOAD + UGT + jit_oob_trap +
  // UNREACHABLE: the extra control flow and unreachable block tickles a
  // non-terminating linear scan in thirdparty ir_reg_alloc at O1/O2.
  ir_ref EnvPtrVal = ensureValidRef(EnvPtr, IR_ADDR);
  ir_ref BaseU32 = coerceToType(Base, IR_U32);
  uint8_t ProtoParams[4] = {IR_ADDR, IR_U32, IR_U32, IR_U32};
  ir_ref Proto =
      ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 4, ProtoParams);
  ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_bounds_check, Proto);
  ir_CALL_4(IR_VOID, Fn, EnvPtrVal, BaseU32,
            ir_CONST_I32(static_cast<int32_t>(Offset)),
            ir_CONST_I32(static_cast<int32_t>(AccessSize)));
}

/// Return the number of bytes accessed by a memory load/store opcode.
static uint32_t getMemoryAccessSize(OpCode Op) {
  switch (Op) {
  case OpCode::I32__load:    case OpCode::I32__store:
  case OpCode::F32__load:    case OpCode::F32__store:
  case OpCode::I64__load32_s: case OpCode::I64__load32_u:
  case OpCode::I64__store32:
    return 4;
  case OpCode::I64__load:    case OpCode::I64__store:
  case OpCode::F64__load:    case OpCode::F64__store:
    return 8;
  case OpCode::I32__load16_s: case OpCode::I32__load16_u:
  case OpCode::I32__store16:
  case OpCode::I64__load16_s: case OpCode::I64__load16_u:
  case OpCode::I64__store16:
    return 2;
  case OpCode::I32__load8_s: case OpCode::I32__load8_u:
  case OpCode::I32__store8:
  case OpCode::I64__load8_s: case OpCode::I64__load8_u:
  case OpCode::I64__store8:
    return 1;
  default:
    return 0;
  }
}

Expect<void> WasmToIRBuilder::visitMemory(const AST::Instruction &Instr) {
  OpCode Op = Instr.getOpCode();
  uint32_t Offset = Instr.getMemoryOffset();
  (void)Instr.getMemoryAlign(); // Alignment hints not enforced

  ir_ctx *ctx = &Ctx; // For IR macros
  ir_ref Result = IR_UNUSED;

  // Check if this is a load or store operation
  bool IsLoad = (Op >= OpCode::I32__load && Op <= OpCode::I64__load32_u);

  if (IsLoad) {
    // Load operations: pop address, load from memory, push result
    ir_ref BaseAddr = pop(); // Address from stack (i32)
    buildBoundsCheck(BaseAddr, Offset, getMemoryAccessSize(Op));
    ir_ref EffectiveAddr = buildMemoryAddress(BaseAddr, Offset);

    switch (Op) {
    // Full-width loads
    case OpCode::I32__load:
      Result = ir_LOAD_I32(EffectiveAddr);
      break;
    case OpCode::I64__load:
      Result = ir_LOAD_I64(EffectiveAddr);
      break;
    case OpCode::F32__load:
      Result = ir_LOAD_F(EffectiveAddr);
      break;
    case OpCode::F64__load:
      Result = ir_LOAD_D(EffectiveAddr);
      break;

    // i32 partial loads - sign extend
    case OpCode::I32__load8_s: {
      ir_ref Loaded = ir_LOAD_I8(EffectiveAddr);
      Result = ir_SEXT_I32(Loaded);
      break;
    }
    case OpCode::I32__load16_s: {
      ir_ref Loaded = ir_LOAD_I16(EffectiveAddr);
      Result = ir_SEXT_I32(Loaded);
      break;
    }

    // i32 partial loads - zero extend
    case OpCode::I32__load8_u: {
      ir_ref Loaded = ir_LOAD_U8(EffectiveAddr);
      Result = ir_ZEXT_I32(Loaded);
      break;
    }
    case OpCode::I32__load16_u: {
      ir_ref Loaded = ir_LOAD_U16(EffectiveAddr);
      Result = ir_ZEXT_I32(Loaded);
      break;
    }

    // i64 partial loads - sign extend
    case OpCode::I64__load8_s: {
      ir_ref Loaded = ir_LOAD_I8(EffectiveAddr);
      Result = ir_SEXT_I64(Loaded);
      break;
    }
    case OpCode::I64__load16_s: {
      ir_ref Loaded = ir_LOAD_I16(EffectiveAddr);
      Result = ir_SEXT_I64(Loaded);
      break;
    }
    case OpCode::I64__load32_s: {
      ir_ref Loaded = ir_LOAD_I32(EffectiveAddr);
      Result = ir_SEXT_I64(Loaded);
      break;
    }

    // i64 partial loads - zero extend
    case OpCode::I64__load8_u: {
      ir_ref Loaded = ir_LOAD_U8(EffectiveAddr);
      Result = ir_ZEXT_I64(Loaded);
      break;
    }
    case OpCode::I64__load16_u: {
      ir_ref Loaded = ir_LOAD_U16(EffectiveAddr);
      Result = ir_ZEXT_I64(Loaded);
      break;
    }
    case OpCode::I64__load32_u: {
      ir_ref Loaded = ir_LOAD_U32(EffectiveAddr);
      Result = ir_ZEXT_I64(Loaded);
      break;
    }

    default:
      return Unexpect(ErrCode::Value::RuntimeError);
    }

    push(Result);
  } else {
    // Store operations: pop value, pop address, store to memory
    ir_ref Value = pop();      // Value to store
    ir_ref BaseAddr = pop();   // Address from stack (i32)
    buildBoundsCheck(BaseAddr, Offset, getMemoryAccessSize(Op));
    ir_ref EffectiveAddr = buildMemoryAddress(BaseAddr, Offset);

    switch (Op) {
    // Full-width stores
    case OpCode::I32__store:
      ir_STORE(EffectiveAddr, Value);
      break;
    case OpCode::I64__store:
      ir_STORE(EffectiveAddr, Value);
      break;
    case OpCode::F32__store:
      ir_STORE(EffectiveAddr, Value);
      break;
    case OpCode::F64__store:
      ir_STORE(EffectiveAddr, Value);
      break;

    // Partial stores - truncate and store
    // Note: Only truncate if source is larger than target; otherwise use value as-is
    case OpCode::I32__store8: {
      // Truncate i32 to i8 and store - i32 is always >= i8
      ir_ref Truncated = ir_TRUNC_U8(Value);
      ir_STORE(EffectiveAddr, Truncated);
      break;
    }
    case OpCode::I32__store16: {
      // Truncate i32 to i16 and store - i32 is always >= i16
      ir_ref Truncated = ir_TRUNC_U16(Value);
      ir_STORE(EffectiveAddr, Truncated);
      break;
    }
    case OpCode::I64__store8: {
      // Truncate i64 to i8 and store - i64 is always >= i8
      ir_ref Truncated = ir_TRUNC_U8(Value);
      ir_STORE(EffectiveAddr, Truncated);
      break;
    }
    case OpCode::I64__store16: {
      // Truncate i64 to i16 and store - i64 is always >= i16
      ir_ref Truncated = ir_TRUNC_U16(Value);
      ir_STORE(EffectiveAddr, Truncated);
      break;
    }
    case OpCode::I64__store32: {
      // Truncate i64 to i32 - only truncate if value is actually i64
      ir_type vtype = IR_I64;  // Expected type
      if (Value > 0 && Value < static_cast<ir_ref>(Ctx.insns_count)) {
        vtype = static_cast<ir_type>(Ctx.ir_base[Value].type);
      }
      if (vtype == IR_I64 || vtype == IR_U64 || vtype == IR_ADDR) {
        ir_ref Truncated = ir_TRUNC_I32(Value);
        ir_STORE(EffectiveAddr, Truncated);
      } else {
        // Value is already i32 or smaller
        ir_STORE(EffectiveAddr, Value);
      }
      break;
    }

    default:
      return Unexpect(ErrCode::Value::RuntimeError);
    }
  }

  return {};
}

Expect<void> WasmToIRBuilder::visitRefType(const AST::Instruction &Instr) {
  OpCode Op = Instr.getOpCode();
  ir_ctx *ctx = &Ctx;

  switch (Op) {
  case OpCode::Ref__null: {
    // Push null ref: RefVariant(type, nullptr) as two i64 slots (type, ptr).
    WasmEdge::RefVariant rv(Instr.getValType());
    WasmEdge::uint64x2_t raw = rv.getRawData();
    pushRef(ir_CONST_I64(static_cast<int64_t>(raw[0])),
            ir_CONST_I64(static_cast<int64_t>(raw[1])));
    return {};
  }
  case OpCode::Ref__is_null: {
    auto [PtrRef, TypeRef] = popRef();
    // ref.is_null: 1 if ptr is null, else 0. ir_EQ returns BOOL; convert to i32.
    ir_ref PtrI32 = ir_TRUNC_I32(ensureValidRef(PtrRef, IR_I64));
    ir_ref Cmp = ir_EQ(PtrI32, ir_CONST_I32(0));
    push(ir_COND(IR_I32, Cmp, ir_CONST_I32(1), ir_CONST_I32(0)));
    return {};
  }
  case OpCode::Ref__func: {
    // Call jit_ref_func(env, funcIdx); result written to env->RefResultBuf. Load and push.
    uint32_t funcIdx = Instr.getTargetIndex();
    ir_ref EnvPtrVal = ensureValidRef(EnvPtr, IR_ADDR);
    ir_ref ResultBuf =
        ir_ADD_A(EnvPtrVal, ir_CONST_ADDR(offsetof(JitExecEnv, RefResultBuf)));
    uint8_t ProtoParams[2] = {IR_ADDR, IR_U32};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 2, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_ref_func, Proto);
    ir_CALL_2(IR_VOID, Fn, EnvPtrVal, ir_CONST_I32(static_cast<int32_t>(funcIdx)));
    ir_ref TypePart = ir_LOAD_I64(ResultBuf);
    ir_ref PtrPart =
        ir_LOAD_I64(ir_ADD_A(ResultBuf, ir_CONST_ADDR(sizeof(uint64_t))));
    pushRef(TypePart, PtrPart);
    return {};
  }
  default:
    return Unexpect(ErrCode::Value::RuntimeError);
  }
}

Expect<void> WasmToIRBuilder::visitTable(const AST::Instruction &Instr) {
  OpCode Op = Instr.getOpCode();
  ir_ctx *ctx = &Ctx;
  uint32_t tableIdx = Instr.getTargetIndex();
  uint32_t elemIdx = Instr.getSourceIndex();
  ir_ref EnvPtrVal = ensureValidRef(EnvPtr, IR_ADDR);
  ir_ref ResultBuf =
      ir_ADD_A(EnvPtrVal, ir_CONST_ADDR(offsetof(JitExecEnv, RefResultBuf)));

  switch (Op) {
  case OpCode::Table__get: {
    ir_ref Idx = ensureValidRef(pop(), IR_I32);
    uint8_t ProtoParams[3] = {IR_ADDR, IR_U32, IR_U32};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 3, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_table_get, Proto);
    ir_CALL_3(IR_VOID, Fn, EnvPtrVal, ir_CONST_I32(static_cast<int32_t>(tableIdx)),
              coerceToType(Idx, IR_U32));
    pushRef(ir_LOAD_I64(ResultBuf),
            ir_LOAD_I64(ir_ADD_A(ResultBuf, ir_CONST_ADDR(sizeof(uint64_t)))));
    return {};
  }
  case OpCode::Table__set: {
    auto [PtrRef, TypeRef] = popRef();
    ir_ref Idx = ensureValidRef(pop(), IR_I32);
    // Store ref to a temp (ResultBuf), then call jit_table_set(env, tableIdx, idx, buf).
    ir_STORE(ResultBuf, TypeRef);
    ir_STORE(ir_ADD_A(ResultBuf, ir_CONST_ADDR(sizeof(uint64_t))), PtrRef);
    uint8_t ProtoParams[4] = {IR_ADDR, IR_U32, IR_U32, IR_ADDR};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 4, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_table_set, Proto);
    ir_CALL_4(IR_VOID, Fn, EnvPtrVal,
              ir_CONST_I32(static_cast<int32_t>(tableIdx)), coerceToType(Idx, IR_U32),
              ResultBuf);
    return {};
  }
  case OpCode::Table__size: {
    uint8_t ProtoParams[2] = {IR_ADDR, IR_U32};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_I32, 2, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_table_size, Proto);
    ir_ref Result = ir_CALL_2(IR_I32, Fn, EnvPtrVal,
                              ir_CONST_I32(static_cast<int32_t>(tableIdx)));
    push(Result);
    return {};
  }
  case OpCode::Table__grow: {
    auto [PtrRef, TypeRef] = popRef();
    ir_ref N = ensureValidRef(pop(), IR_I32);
    ir_STORE(ResultBuf, TypeRef);
    ir_STORE(ir_ADD_A(ResultBuf, ir_CONST_ADDR(sizeof(uint64_t))), PtrRef);
    uint8_t ProtoParams[4] = {IR_ADDR, IR_U32, IR_U32, IR_ADDR};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_I32, 4, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_table_grow, Proto);
    ir_ref Result = ir_CALL_4(IR_I32, Fn, EnvPtrVal,
                              ir_CONST_I32(static_cast<int32_t>(tableIdx)),
                              coerceToType(N, IR_U32), ResultBuf);
    push(Result);
    return {};
  }
  case OpCode::Table__fill: {
    ir_ref Len = ensureValidRef(pop(), IR_I32);
    auto [PtrRef, TypeRef] = popRef();
    ir_ref Off = ensureValidRef(pop(), IR_I32);
    ir_STORE(ResultBuf, TypeRef);
    ir_STORE(ir_ADD_A(ResultBuf, ir_CONST_ADDR(sizeof(uint64_t))), PtrRef);
    uint8_t ProtoParams[5] = {IR_ADDR, IR_U32, IR_U32, IR_U32, IR_ADDR};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 5, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_table_fill, Proto);
    ir_CALL_5(IR_VOID, Fn, EnvPtrVal,
              ir_CONST_I32(static_cast<int32_t>(tableIdx)), coerceToType(Off, IR_U32),
              coerceToType(Len, IR_U32), ResultBuf);
    return {};
  }
  case OpCode::Table__copy: {
    uint32_t dstTableIdx = tableIdx;
    uint32_t srcTableIdx = elemIdx;  // getSourceIndex() is src table for copy
    ir_ref Len = ensureValidRef(pop(), IR_I32);
    ir_ref Src = ensureValidRef(pop(), IR_I32);
    ir_ref Dst = ensureValidRef(pop(), IR_I32);
    uint8_t ProtoParams[6] = {IR_ADDR, IR_U32, IR_U32, IR_U32, IR_U32, IR_U32};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 6, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_table_copy, Proto);
    ir_CALL_6(IR_VOID, Fn, EnvPtrVal,
              ir_CONST_I32(static_cast<int32_t>(dstTableIdx)),
              ir_CONST_I32(static_cast<int32_t>(srcTableIdx)),
              coerceToType(Dst, IR_U32), coerceToType(Src, IR_U32),
              coerceToType(Len, IR_U32));
    return {};
  }
  case OpCode::Table__init: {
    ir_ref Len = ensureValidRef(pop(), IR_I32);
    ir_ref Src = ensureValidRef(pop(), IR_I32);
    ir_ref Dst = ensureValidRef(pop(), IR_I32);
    uint8_t ProtoParams[6] = {IR_ADDR, IR_U32, IR_U32, IR_U32, IR_U32, IR_U32};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 6, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_table_init, Proto);
    ir_CALL_6(IR_VOID, Fn, EnvPtrVal,
              ir_CONST_I32(static_cast<int32_t>(tableIdx)),
              ir_CONST_I32(static_cast<int32_t>(elemIdx)),
              coerceToType(Dst, IR_U32), coerceToType(Src, IR_U32),
              coerceToType(Len, IR_U32));
    return {};
  }
  case OpCode::Elem__drop: {
    uint8_t ProtoParams[2] = {IR_ADDR, IR_U32};
    ir_ref Proto = ir_proto(ctx, IR_FASTCALL_FUNC, IR_VOID, 2, ProtoParams);
    ir_ref Fn = ir_const_func_addr(ctx, (uintptr_t)&jit_elem_drop, Proto);
    ir_CALL_2(IR_VOID, Fn, EnvPtrVal,
              ir_CONST_I32(static_cast<int32_t>(Instr.getTargetIndex())));
    return {};
  }
  default:
    return Unexpect(ErrCode::Value::RuntimeError);
  }
}

Expect<void> WasmToIRBuilder::visitControl(const AST::Instruction &Instr) {
  OpCode Op = Instr.getOpCode();

  switch (Op) {
  case OpCode::Nop:
    // Nop does nothing
    return {};
  case OpCode::Unreachable: {
    // Unreachable – Wasm semantics say "trap here".
    // We emit ir_RETURN (not ir_UNREACHABLE) so the IR backend always
    // generates a proper function epilogue.  ir_UNREACHABLE marks the
    // successor block as dead code and the code emitter generates nothing
    // after a preceding CALL, causing fall-through into uninitialised
    // memory.  Returning a zero/default value is safe: the Wasm-level
    // trap is still reported by the interpreter for functions that are
    // reached through the host-call trampoline.
    ir_ctx *ctx = &Ctx;
    ir_RETURN(getOrEmitReturnValue());
    CurrentPathTerminated = true;
    return {};
  }
  case OpCode::Block:
    return visitBlock(Instr);
  case OpCode::Loop:
    return visitLoop(Instr);
  case OpCode::If:
    return visitIf(Instr);
  case OpCode::Else:
    return visitElse(Instr);
  case OpCode::End:
    return visitEnd(Instr);
  case OpCode::Br:
    return visitBr(Instr);
  case OpCode::Br_if:
    return visitBrIf(Instr);
  case OpCode::Br_table:
    return visitBrTable(Instr);
  case OpCode::Return:
    return visitReturn(Instr);
  default:
    return Unexpect(ErrCode::Value::RuntimeError);
  }
}

void WasmToIRBuilder::mergeRefResults(
    const std::vector<std::pair<ir_ref, ir_ref>> &RefBranchResults) {
  std::vector<ir_ref> TypeVals, PtrVals;
  TypeVals.reserve(RefBranchResults.size());
  PtrVals.reserve(RefBranchResults.size());
  for (const auto &[TypeRef, PtrRef] : RefBranchResults) {
    TypeVals.push_back(TypeRef);
    PtrVals.push_back(PtrRef);
  }
  pushRef(emitPhi(IR_I64, TypeVals), emitPhi(IR_I64, PtrVals));
}

ir_type WasmToIRBuilder::getLocalType(uint32_t LocalIdx) const noexcept {
  auto it = LocalTypes.find(LocalIdx);
  assert(it != LocalTypes.end() &&
         "local missing from LocalTypes — should be populated at function entry");
  return it->second;
}

void WasmToIRBuilder::finalizeMerge(LabelInfo &Label) {
  ir_ctx *ctx = &Ctx;
  size_t NumPaths = Label.EndList.size();

  if (NumPaths == 0) {
    return;
  }

  if (NumPaths == 1) {
    ir_BEGIN(Label.EndList[0]);
    if (!Label.EndLocals.empty()) {
      Locals = Label.EndLocals[0];
    }
    if (Label.Arity > 0) {
      if (Label.ResultIsRef && !Label.RefBranchResults.empty()) {
        pushRef(Label.RefBranchResults[0].first,
                Label.RefBranchResults[0].second);
      } else if (!Label.BranchResults.empty()) {
        push(Label.BranchResults[0]);
      }
    }
  } else {
    if (NumPaths == 2) {
      ir_MERGE_2(Label.EndList[0], Label.EndList[1]);
    } else {
      ir_MERGE_N(static_cast<ir_ref>(NumPaths), Label.EndList.data());
    }
    if (!Label.EndLocals.empty()) {
      Locals = mergeLocals(Label.EndLocals);
    }
    if (Label.Arity > 0 && !Label.ResultIsRef &&
        Label.BranchResults.size() == NumPaths) {
      mergeResults(Label.BranchResults, Label.ResultType);
    } else if (Label.Arity > 0 &&
               Label.RefBranchResults.size() == NumPaths) {
      mergeRefResults(Label.RefBranchResults);
    }
  }
  CurrentPathTerminated = false;
}

ir_ref WasmToIRBuilder::emitPhi(ir_type Type, std::vector<ir_ref> &Vals) {
  ir_ctx *ctx = &Ctx;
  assert(Vals.size() >= 2 && "emitPhi requires at least 2 values");
  if (Vals.size() == 2) {
    return ir_PHI_2(Type, Vals[0], Vals[1]);
  }
  return ir_PHI_N(Type, static_cast<ir_ref>(Vals.size()), Vals.data());
}

std::map<uint32_t, ir_ref> WasmToIRBuilder::mergeLocals(
    const std::vector<std::map<uint32_t, ir_ref>> &EndLocals) {
  size_t NumPaths = EndLocals.size();

  // Collect the union of all local indices across all paths.
  // Paths may have different key sets when a previous merge dropped locals.
  std::set<uint32_t> AllLocalIndices;
  for (const auto &PathLocals : EndLocals) {
    for (const auto &[Idx, _] : PathLocals) {
      AllLocalIndices.insert(Idx);
    }
  }

  std::map<uint32_t, ir_ref> Merged;
  for (uint32_t LocalIdx : AllLocalIndices) {
    // Find a representative value from any path that has this local.
    ir_ref FirstVal = IR_UNUSED;
    for (const auto &PathLocals : EndLocals) {
      auto it = PathLocals.find(LocalIdx);
      if (it != PathLocals.end()) {
        FirstVal = it->second;
        break;
      }
    }
    if (FirstVal == IR_UNUSED) {
      continue;
    }
    std::vector<ir_ref> Values;
    bool AllSame = true;
    Values.reserve(NumPaths);
    for (const auto &PathLocals : EndLocals) {
      auto it = PathLocals.find(LocalIdx);
      if (it != PathLocals.end()) {
        if (it->second != FirstVal) {
          AllSame = false;
        }
        Values.push_back(it->second);
      } else {
        // Local missing on this path — use value from another path so PHI
        // input count matches MERGE and we don't drop the local.
        Values.push_back(FirstVal);
      }
    }
    if (AllSame) {
      Merged[LocalIdx] = FirstVal;
    } else {
      ir_type LocalType = getLocalType(LocalIdx);
      std::vector<ir_ref> Coerced;
      Coerced.reserve(NumPaths);
      for (ir_ref V : Values) {
        Coerced.push_back(coerceToType(V, LocalType));
      }
      Merged[LocalIdx] = emitPhi(LocalType, Coerced);
    }
  }
  return Merged;
}

void WasmToIRBuilder::mergeResults(const std::vector<ir_ref> &BranchResults,
                                   ir_type ResultType) {
  if (BranchResults.size() == 1) {
    push(BranchResults[0]);
  } else {
    std::vector<ir_ref> Coerced;
    Coerced.reserve(BranchResults.size());
    for (ir_ref R : BranchResults) {
      Coerced.push_back(coerceToType(R, ResultType));
    }
    push(emitPhi(ResultType, Coerced));
  }
}

void WasmToIRBuilder::emitLoopBackEdge(LabelInfo &Target) {
  ir_ctx *ctx = &Ctx;

  std::map<uint32_t, ir_ref> BackEdgeLocals;
  for (const auto &[LocalIdx, _] : Target.LoopLocalPhis) {
    auto it = Locals.find(LocalIdx);
    if (it == Locals.end()) {
      continue;
    }
    BackEdgeLocals[LocalIdx] = coerceToType(it->second, getLocalType(LocalIdx));
  }

  ir_ref BackEnd = ir_END();
  Target.LoopBackEdgeEnds.push_back(BackEnd);
  Target.LoopBackEdgeLocals.push_back(std::move(BackEdgeLocals));
  Target.BackEdgeEmitted = true;
}

Expect<void> WasmToIRBuilder::visitBlock(const AST::Instruction &Instr) {
  // Create label info for the block
  // Blocks are forward-jump targets: br jumps to AFTER the end
  LabelInfo Label;
  Label.Kind = ControlKind::Block;
  Label.LoopHeader = IR_UNUSED;
  Label.IfRef = IR_UNUSED;
  Label.ElseEnd = IR_UNUSED;
  
  // Get result type from block type
  const BlockType &BType = Instr.getBlockType();
  if (!BType.isEmpty() && BType.isValType()) {
    Label.Arity = 1;
    Label.ResultType = wasmTypeToIRType(BType.getValType());
    Label.ResultIsRef = BType.getValType().isRefType();
  } else {
    Label.Arity = 0;
    Label.ResultType = IR_VOID;
  }
  
  Label.StackBase = static_cast<uint32_t>(ValueStack.size());
  Label.InElseBranch = false;
  Label.HasElse = false;
  Label.TrueBranchTerminated = false;
  Label.ElseBranchTerminated = false;
  // EndList will collect ir_END() refs from branches that need to merge

  LabelStack.push_back(Label);
  return {};
}

Expect<void> WasmToIRBuilder::visitLoop(const AST::Instruction &Instr) {
  ir_ctx *ctx = &Ctx;
  
  ir_ref LoopEntry = ir_END();
  ir_ref LoopHeader = ir_LOOP_BEGIN(LoopEntry);
  
  LabelInfo Label;
  Label.Kind = ControlKind::Loop;
  Label.LoopHeader = LoopHeader;
  Label.IfRef = IR_UNUSED;
  Label.ElseEnd = IR_UNUSED;

  const BlockType &BType = Instr.getBlockType();
  if (!BType.isEmpty() && BType.isValType()) {
    Label.Arity = 1;
    Label.ResultType = wasmTypeToIRType(BType.getValType());
    Label.ResultIsRef = BType.getValType().isRefType();
  } else {
    Label.Arity = 0;
    Label.ResultType = IR_VOID;
  }

  Label.StackBase = static_cast<uint32_t>(ValueStack.size());
  Label.InElseBranch = false;
  Label.HasElse = false;
  Label.TrueBranchTerminated = false;
  Label.ElseBranchTerminated = false;

  // PHI + COPY only for locals written (local.set / local.tee) in the loop
  // body; others keep the pre-loop SSA value for all iterations.
  std::unordered_set<uint32_t> WrittenLocals;
  {
    uint32_t JE = Instr.getJumpEnd();
    if (JE >= 1 && CurInstrIdx + JE <= FuncInstrs.size()) {
      collectLocalWritesInSpan(FuncInstrs.subspan(CurInstrIdx + 1, JE - 1),
                               WrittenLocals);
    }
  }

  for (auto &[LocalIdx, LocalRef] : Locals) {
    if (!WrittenLocals.count(LocalIdx)) {
      continue;
    }
    Label.PreLoopLocals[LocalIdx] = LocalRef;
    ir_type LocalType = getLocalType(LocalIdx);
    // Create PHI_2: first operand is pre-loop value, second will be set at back-edge
    ir_ref CoercedRef = coerceToType(LocalRef, LocalType);
    ir_ref Phi = ir_PHI_2(LocalType, CoercedRef, IR_UNUSED);
    
    // Create a COPY of the PHI to materialize its value at the start of each iteration.
    // This ensures we have a concrete SSA value to use when exiting the loop early.
    ir_ref PhiCopy = ir_COPY(LocalType, Phi);

    Label.LoopLocalPhis[LocalIdx] = Phi;
    
    // Update Locals to use the COPY (materialized PHI value) inside the loop
    Locals[LocalIdx] = PhiCopy;
  }

  LabelStack.push_back(Label);
  return {};
}

Expect<void> WasmToIRBuilder::visitIf(const AST::Instruction &Instr) {
  ir_ctx *ctx = &Ctx;
  ir_ref Condition = pop();
  
  // Create IF node
  ir_ref IfRef = ir_IF(Condition);
  
  // Start the true branch
  ir_IF_TRUE(IfRef);
  
  // Reset termination flag - we're starting a new live branch
  CurrentPathTerminated = false;
  
  LabelInfo Label;
  Label.Kind = ControlKind::If;
  Label.LoopHeader = IR_UNUSED;
  Label.IfRef = IfRef;
  Label.ElseEnd = IR_UNUSED;
  
  // Get result type from block type
  const BlockType &BType = Instr.getBlockType();
  if (!BType.isEmpty() && BType.isValType()) {
    Label.Arity = 1;
    Label.ResultType = wasmTypeToIRType(BType.getValType());
    Label.ResultIsRef = BType.getValType().isRefType();
  } else {
    Label.Arity = 0;
    Label.ResultType = IR_VOID;
  }
  
  Label.StackBase = static_cast<uint32_t>(ValueStack.size());
  Label.InElseBranch = false;
  Label.HasElse = false;
  Label.TrueBranchTerminated = false;
  Label.ElseBranchTerminated = false;
  
  // Save locals state before entering if (needed for PHIs at merge)
  Label.PreIfLocals = Locals;

  LabelStack.push_back(Label);
  return {};
}

Expect<void> WasmToIRBuilder::visitElse(const AST::Instruction &) {
  ir_ctx *ctx = &Ctx;
  
  if (LabelStack.empty()) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }
  
  LabelInfo &Label = LabelStack.back();
  if (Label.Kind != ControlKind::If) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }
  
  // Record if the true branch terminated (via return/unreachable)
  Label.TrueBranchTerminated = CurrentPathTerminated;
  
  // Save result value from true branch (if there's a result type and branch didn't terminate)
  if (Label.Arity > 0 && !CurrentPathTerminated) {
    if (Label.ResultIsRef) {
      auto [ptrRef, typeRef] = popRef();
      Label.RefBranchResults.push_back({typeRef, ptrRef});
    } else {
      ir_ref TrueResult = pop();
      Label.BranchResults.push_back(TrueResult);
    }
  }
  
  // Save the true branch's locals state (for PHI creation at merge)
  if (!CurrentPathTerminated) {
    Label.EndLocals.push_back(Locals);
    ir_ref TrueEnd = ir_END();
    Label.EndList.push_back(TrueEnd);
  }
  
  // Start the false branch
  ir_IF_FALSE(Label.IfRef);
  
  // Restore pre-if locals for the else branch
  // (else branch should start with the same state as before the if)
  Locals = Label.PreIfLocals;
  
  // Reset termination flag for the else branch
  CurrentPathTerminated = false;
  
  Label.InElseBranch = true;
  Label.HasElse = true;
  
  return {};
}

Expect<void> WasmToIRBuilder::visitEnd(const AST::Instruction &) {
  ir_ctx *ctx = &Ctx;
  
  if (LabelStack.empty()) {
    // Function end - only add return if current path is still live
    if (CurrentPathTerminated) {
      // All paths already returned/terminated, don't generate another return
      return {};
    }
    ir_RETURN(getOrEmitReturnValue());
    return {};
  }

  LabelInfo &Label = LabelStack.back();
  
  switch (Label.Kind) {
  case ControlKind::Block: {
    // Block end: merge all branches that targeted this block
    // Only add current path's END if it wasn't already terminated
    if (!CurrentPathTerminated) {
      if (Label.Arity > 0) {
        if (Label.ResultIsRef) {
          auto [ptrRef, typeRef] = popRef();
          Label.RefBranchResults.push_back({typeRef, ptrRef});
        } else {
          Label.BranchResults.push_back(pop());
        }
      }
      ir_ref CurrentEnd = ir_END();
      Label.EndList.push_back(CurrentEnd);
      Label.EndLocals.push_back(Locals);  // Save current Locals
    }
    
    finalizeMerge(Label);
    break;
  }
  
  case ControlKind::Loop: {
    // Finalize back-edges: merge all collected back-edge ENDs into a
    // single LOOP_END and wire it to the LOOP_BEGIN.
    if (!Label.LoopBackEdgeEnds.empty()) {
      // Save the fall-through path (if not terminated)
      ir_ref FallthroughEnd = IR_UNUSED;
      if (!CurrentPathTerminated) {
        FallthroughEnd = ir_END();
      }

      size_t NumBackEdges = Label.LoopBackEdgeEnds.size();
      if (NumBackEdges == 1) {
        ir_BEGIN(Label.LoopBackEdgeEnds[0]);
      } else {
        ir_MERGE_N(static_cast<ir_ref>(NumBackEdges),
                    Label.LoopBackEdgeEnds.data());
      }

      // Wire PHI values for loop locals
      for (auto &[LocalIdx, PhiRef] : Label.LoopLocalPhis) {
        if (NumBackEdges == 1) {
          auto it = Label.LoopBackEdgeLocals[0].find(LocalIdx);
          if (it != Label.LoopBackEdgeLocals[0].end()) {
            ir_PHI_SET_OP(PhiRef, 2, it->second);
          } else {
            // Unmodified: wire to pre-loop value (PHI becomes dead below).
            auto preIt = Label.PreLoopLocals.find(LocalIdx);
            assert(preIt != Label.PreLoopLocals.end() &&
                   "LoopLocalPhis entry must have corresponding PreLoopLocals entry");
            ir_PHI_SET_OP(PhiRef, 2, preIt->second);
          }
        } else {
          // Multiple back-edges: create intermediate PHI at the merge
          std::vector<ir_ref> Vals;
          Vals.reserve(NumBackEdges);
          for (size_t i = 0; i < NumBackEdges; ++i) {
            auto it = Label.LoopBackEdgeLocals[i].find(LocalIdx);
            if (it != Label.LoopBackEdgeLocals[i].end()) {
              Vals.push_back(it->second);
            } else {
              // Unmodified on this back-edge: use the pre-loop value
              auto preIt = Label.PreLoopLocals.find(LocalIdx);
              assert(preIt != Label.PreLoopLocals.end() &&
                     "LoopLocalPhis entry must have corresponding PreLoopLocals entry");
              Vals.push_back(preIt->second);
            }
          }
          ir_PHI_SET_OP(PhiRef, 2, emitPhi(getLocalType(LocalIdx), Vals));
        }
      }

      ir_ref LoopEnd = ir_LOOP_END();
      ir_MERGE_SET_OP(Label.LoopHeader, 2, LoopEnd);

      // Restore the fall-through path
      if (FallthroughEnd != IR_UNUSED) {
        ir_BEGIN(FallthroughEnd);
        CurrentPathTerminated = false;
      } else {
        CurrentPathTerminated = true;
      }
    }
    break;
  }
  
  case ControlKind::If: {
    // Record branch termination status and save locals
    if (Label.HasElse) {
      // We're at the end of the else branch
      Label.ElseBranchTerminated = CurrentPathTerminated;
      // Save else branch locals (if not terminated)
      if (!CurrentPathTerminated) {
        Label.EndLocals.push_back(Locals);
      }
    } else {
      // No else: we're at the end of the true branch
      // Record true branch termination status now (since visitElse wasn't called)
      Label.TrueBranchTerminated = CurrentPathTerminated;
      // Save true branch locals (if not terminated)
      if (!CurrentPathTerminated) {
        Label.EndLocals.push_back(Locals);
      }
      // "else" path is just fallthrough (not terminated)
      Label.ElseBranchTerminated = false;
    }
    
    // If both branches terminated, the code after if is unreachable
    if (Label.TrueBranchTerminated && Label.ElseBranchTerminated) {
      // Mark current path as terminated - code after this if is dead
      CurrentPathTerminated = true;
      // Don't create merge - both paths are dead
      break;
    }
    
    // Save result value from current branch (if there's a result type and branch didn't terminate)
    if (Label.Arity > 0 && !CurrentPathTerminated) {
      if (Label.ResultIsRef) {
        auto [ptrRef, typeRef] = popRef();
        Label.RefBranchResults.push_back({typeRef, ptrRef});
      } else {
        ir_ref BranchResult = pop();
        Label.BranchResults.push_back(BranchResult);
      }
    }
    
    // Only add current end if branch didn't terminate
    if (!CurrentPathTerminated) {
      ir_ref CurrentEnd = ir_END();
      Label.EndList.push_back(CurrentEnd);
    }
    
    if (!Label.HasElse) {
      // No else clause: create empty false branch
      // (execution falls through when condition is false)
      ir_IF_FALSE(Label.IfRef);
      
      // False branch has the pre-if locals (unmodified by true branch)
      Label.EndLocals.push_back(Label.PreIfLocals);
      
      if (Label.Arity > 0) {
        if (Label.ResultIsRef) {
          // Default null ref (type part, ptr part)
          Label.RefBranchResults.push_back({ir_CONST_I64(0), ir_CONST_I64(0)});
        } else {
          ir_ref DefaultVal = IR_UNUSED;
          if (Label.ResultType == IR_I32) {
            DefaultVal = ir_CONST_I32(0);
          } else if (Label.ResultType == IR_I64) {
            DefaultVal = ir_CONST_I64(0);
          } else if (Label.ResultType == IR_FLOAT) {
            DefaultVal = ir_CONST_FLOAT(0.0f);
          } else if (Label.ResultType == IR_DOUBLE) {
            DefaultVal = ir_CONST_DOUBLE(0.0);
          }
          Label.BranchResults.push_back(DefaultVal);
        }
      }
      ir_ref FalseEnd = ir_END();
      Label.EndList.push_back(FalseEnd);
    }
    
    finalizeMerge(Label);
    break;
  }
  }

  LabelStack.pop_back();
  return {};
}

Expect<void> WasmToIRBuilder::visitBr(const AST::Instruction &Instr) {
  ir_ctx *ctx = &Ctx;
  uint32_t LabelIdx = Instr.getTargetIndex();

  if (LabelIdx >= LabelStack.size()) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // Get the target label (index 0 is innermost)
  LabelInfo &Target = LabelStack[LabelStack.size() - 1 - LabelIdx];
  
  if (Target.Kind == ControlKind::Loop) {
    emitLoopBackEdge(Target);
  } else {
    if (Target.Arity > 0) {
      if (Target.ResultIsRef) {
        auto [ptrRef, typeRef] = popRef();
        Target.RefBranchResults.push_back({typeRef, ptrRef});
      } else {
        Target.BranchResults.push_back(pop());
      }
    }
    ir_ref BrEnd = ir_END();
    Target.EndList.push_back(BrEnd);
    Target.EndLocals.push_back(Locals);
  }
  
  CurrentPathTerminated = true;
  
  return {};
}

Expect<void> WasmToIRBuilder::visitBrIf(const AST::Instruction &Instr) {
  ir_ctx *ctx = &Ctx;
  ir_ref Condition = pop();
  uint32_t LabelIdx = Instr.getTargetIndex();

  if (LabelIdx >= LabelStack.size()) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // Get the target label
  LabelInfo &Target = LabelStack[LabelStack.size() - 1 - LabelIdx];
  
  // Create conditional branch
  ir_ref IfRef = ir_IF(Condition);
  
  // True branch: branch is taken
  ir_IF_TRUE(IfRef);
  
  if (Target.Kind == ControlKind::Loop) {
    emitLoopBackEdge(Target);
  } else {
    if (Target.Arity > 0) {
      if (Target.ResultIsRef) {
        auto [ptrRef, typeRef] = popRef();
        Target.RefBranchResults.push_back({typeRef, ptrRef});
      } else {
        Target.BranchResults.push_back(pop());
      }
    }
    ir_ref BrEnd = ir_END();
    Target.EndList.push_back(BrEnd);
    Target.EndLocals.push_back(Locals);
  }
  
  ir_IF_FALSE(IfRef);
  
  return {};
}

Expect<void> WasmToIRBuilder::visitBrTable(const AST::Instruction &Instr) {
  ir_ctx *ctx = &Ctx;

  // Pop the index value from the stack
  ir_ref IndexVal = pop();

  // Get the label list - last entry is the default
  auto Labels = Instr.getLabelList();
  if (Labels.empty()) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  uint32_t NumCases = static_cast<uint32_t>(Labels.size()) - 1;
  uint32_t DefaultLabelIdx = Labels.back().TargetIndex;

  // Validate default label index
  if (DefaultLabelIdx >= LabelStack.size()) {
    return Unexpect(ErrCode::Value::RuntimeError);
  }

  // Lower br_table to IR switch/case constructs so the backend can emit
  // jump-table style code instead of a long chain of conditional branches.
  ir_ref SwitchRef = ir_SWITCH(IndexVal);

  auto emitBrTableTarget = [this, ctx](LabelInfo &Target) {
    if (Target.Kind == ControlKind::Loop) {
      emitLoopBackEdge(Target);
      return;
    }
    if (Target.Arity > 0) {
      if (Target.ResultIsRef) {
        auto [ptrRef, typeRef] = popRef();
        Target.RefBranchResults.push_back({typeRef, ptrRef});
      } else {
        Target.BranchResults.push_back(pop());
      }
    }
    ir_ref BrEnd = ir_END();
    Target.EndList.push_back(BrEnd);
    Target.EndLocals.push_back(Locals);
  };

  for (uint32_t i = 0; i < NumCases; i++) {
    uint32_t LabelIdx = Labels[i].TargetIndex;
    if (LabelIdx >= LabelStack.size()) {
      return Unexpect(ErrCode::Value::RuntimeError);
    }
    ir_CASE_VAL(SwitchRef, ir_CONST_I32(static_cast<int32_t>(i)));
    LabelInfo &Target = LabelStack[LabelStack.size() - 1 - LabelIdx];
    emitBrTableTarget(Target);
  }

  // Default case (all comparisons failed, or out of bounds).
  ir_CASE_DEFAULT(SwitchRef);
  LabelInfo &DefaultTarget = LabelStack[LabelStack.size() - 1 - DefaultLabelIdx];
  emitBrTableTarget(DefaultTarget);

  // After br_table, code is unreachable (all paths branch away)
  CurrentPathTerminated = true;

  return {};
}

Expect<void> WasmToIRBuilder::visitReturn(const AST::Instruction &) {
  ir_ctx *ctx = &Ctx;
  ir_RETURN(getOrEmitReturnValue());
  CurrentPathTerminated = true;
  return {};
}

//===----------------------------------------------------------------------===//
// Function Call Operations
//===----------------------------------------------------------------------===//

Expect<void> WasmToIRBuilder::visitCall(const AST::Instruction &Instr) {
  ir_ctx *ctx = &Ctx;

  OpCode Op = Instr.getOpCode();

  // Resolve target function type
  const AST::FunctionType *TargetFuncType = nullptr;
  uint32_t ResolvedFuncIdx = 0;

  if (Op == OpCode::Call_indirect) {
    uint32_t TypeIdx = Instr.getTargetIndex();
    if (TypeIdx >= ModuleTypeSection.size())
      return Unexpect(ErrCode::Value::RuntimeError);
    TargetFuncType = ModuleTypeSection[TypeIdx];
  } else {
    ResolvedFuncIdx = Instr.getTargetIndex();
    if (ResolvedFuncIdx >= ModuleFuncTypes.size())
      return Unexpect(ErrCode::Value::RuntimeError);
    TargetFuncType = ModuleFuncTypes[ResolvedFuncIdx];
  }
  if (!TargetFuncType)
    return Unexpect(ErrCode::Value::RuntimeError);

  const auto &ParamTypes = TargetFuncType->getParamTypes();
  const auto &RetTypes = TargetFuncType->getReturnTypes();

  // For call_indirect, pop the runtime table index before popping args.
  ir_ref TableIndex = IR_UNUSED;
  if (Op == OpCode::Call_indirect)
    TableIndex = ensureValidRef(pop(), IR_I32);

  // Pop wasm arguments in reverse order.
  std::vector<ir_ref> WasmArgs(ParamTypes.size());
  for (size_t i = ParamTypes.size(); i > 0; --i) {
    ir_type T = wasmTypeToIRType(ParamTypes[i - 1]);
    WasmArgs[i - 1] = ensureValidRef(pop(), T);
  }

  uint32_t NumArgs = static_cast<uint32_t>(ParamTypes.size());

  // Determine return type.
  ir_type RetType = IR_VOID;
  if (!RetTypes.empty())
    RetType = wasmTypeToIRType(RetTypes[0]);

  ir_ref EnvPtrVal = ensureValidRef(EnvPtr, IR_ADDR);

  // Use host path for direct Call to an import (funcIdx < ImportFuncNum) or
  // to a skipped function (not JIT-compiled, has NULL vtable entry).
  // call_indirect uses the direct path (load from table, jit_direct_or_host) so
  // standalone invoke works without g_jitExecutor; null table entries fall back
  // to jit_host_call via the trampoline.
  bool IsHostCall = (Op == OpCode::Call &&
      (ResolvedFuncIdx < ImportFuncNum ||
       (ResolvedFuncIdx < SkippedFunctions.size() && SkippedFunctions[ResolvedFuncIdx])));

  // High-arity direct JIT calls also use buffer-based ABI (but call the native
  // function directly, not through jit_host_call).
  bool IsHighArityJitCall = (Op == OpCode::Call && !IsHostCall &&
      NumArgs > kRegCallMaxParams);

  if (IsHostCall) {
    // Route through jit_host_call trampoline (buffer-based ABI).
    // Marshal args into the pre-allocated shared buffer.
    ir_ref CalleeArgs = IR_UNUSED;
    if (NumArgs > 0) {
      CalleeArgs = SharedCallArgs;
      for (uint32_t i = 0; i < NumArgs; ++i) {
        ir_ref SlotAddr =
            ir_ADD_A(CalleeArgs, ir_CONST_ADDR(i * sizeof(uint64_t)));
        ir_STORE(SlotAddr, WasmArgs[i]);
      }
    } else {
      CalleeArgs = ir_CONST_ADDR(0);
    }

    // Prototype: uint64_t jit_host_call(JitExecEnv*, uint32_t funcIdx,
    //                                   uint64_t* args)
    ir_ref HCFn = ensureValidRef(HostCallFnPtr, IR_ADDR);
    uint8_t HCProtoParams[3] = {IR_ADDR, IR_I32, IR_ADDR};
    ir_ref HCProto =
        ir_proto(ctx, IR_FASTCALL_FUNC, IR_I64, 3, HCProtoParams);
    ir_ref TypedHC =
        ir_emit2(ctx, IR_OPT(IR_PROTO, IR_ADDR), HCFn, HCProto);

    ir_ref FuncIdxArg = ir_CONST_I32(ResolvedFuncIdx);
    ir_ref HCArgs[3] = {EnvPtrVal, FuncIdxArg, CalleeArgs};
    ir_ref HCResult = ir_CALL_N(IR_I64, TypedHC, 3, HCArgs);

    if (!RetTypes.empty()) {
      if (RetType == IR_I32)
        push(ir_TRUNC_I32(HCResult));
      else if (RetType == IR_I64)
        push(HCResult);
      else if (RetType == IR_FLOAT) {
        ir_ref tmp = ir_ALLOCA(ir_CONST_I32(8));
        ir_STORE(tmp, HCResult);
        push(ir_LOAD_F(tmp));
      } else if (RetType == IR_DOUBLE) {
        ir_ref tmp = ir_ALLOCA(ir_CONST_I32(8));
        ir_STORE(tmp, HCResult);
        push(ir_LOAD_D(tmp));
      } else {
        push(HCResult);
      }
    }
  } else if (Op == OpCode::Call_indirect) {
    // call_indirect: dispatch through jit_call_indirect trampoline (buffer-based ABI).
    // Marshal args into the pre-allocated shared buffer.
    ir_ref CalleeArgs = IR_UNUSED;
    if (NumArgs > 0) {
      CalleeArgs = SharedCallArgs;
      for (uint32_t i = 0; i < NumArgs; ++i) {
        ir_ref SlotAddr =
            ir_ADD_A(CalleeArgs, ir_CONST_ADDR(i * sizeof(uint64_t)));
        ir_STORE(SlotAddr, WasmArgs[i]);
      }
    } else {
      CalleeArgs = ir_CONST_ADDR(0);
    }

    // Resolves table[tableIdx][elemIdx], type-checks against typeIdx, then
    // calls the target (JIT native or interpreter fallback).
    // Prototype: uint64_t jit_call_indirect(JitExecEnv *env, uint32_t tableIdx,
    //                uint32_t elemIdx, uint32_t typeIdx, uint64_t *args,
    //                uint32_t retTypeCode)
    uint32_t TableIdx = Instr.getSourceIndex();
    uint32_t TypeIdx = Instr.getTargetIndex();

    uint32_t retTypeCode = 0;
    if (!RetTypes.empty()) {
      switch (RetType) {
      case IR_I32: retTypeCode = 1; break;
      case IR_I64: retTypeCode = 2; break;
      case IR_FLOAT: retTypeCode = 3; break;
      case IR_DOUBLE: retTypeCode = 4; break;
      default: break;
      }
    }

    ir_ref CIFn = ensureValidRef(CallIndirectFnPtr, IR_ADDR);
    uint8_t CIProtoParams[6] = {IR_ADDR, IR_I32, IR_I32, IR_I32, IR_ADDR, IR_I32};
    ir_ref CIProto =
        ir_proto(ctx, IR_FASTCALL_FUNC, IR_I64, 6, CIProtoParams);
    ir_ref TypedCI =
        ir_emit2(ctx, IR_OPT(IR_PROTO, IR_ADDR), CIFn, CIProto);
    ir_ref CIArgs[6] = {EnvPtrVal,
                        ir_CONST_I32(static_cast<int32_t>(TableIdx)),
                        TableIndex,
                        ir_CONST_I32(static_cast<int32_t>(TypeIdx)),
                        CalleeArgs,
                        ir_CONST_I32(static_cast<int32_t>(retTypeCode))};
    ir_ref CallResult = ir_CALL_N(IR_I64, TypedCI, 6, CIArgs);

    if (!RetTypes.empty()) {
      if (RetType == IR_I32)
        push(ir_TRUNC_I32(CallResult));
      else if (RetType == IR_I64)
        push(CallResult);
      else if (RetType == IR_FLOAT) {
        ir_ref tmp = ir_ALLOCA(ir_CONST_I32(8));
        ir_STORE(tmp, CallResult);
        push(ir_LOAD_F(tmp));
      } else if (RetType == IR_DOUBLE) {
        ir_ref tmp = ir_ALLOCA(ir_CONST_I32(8));
        ir_STORE(tmp, CallResult);
        push(ir_LOAD_D(tmp));
      } else {
        push(CallResult);
      }
    }
  } else if (IsHighArityJitCall) {
    // Direct JIT call with >kRegCallMaxParams params: buffer-based ABI.
    // Marshal args into SharedCallArgs, call as func(env, args_buf).
    ir_ref ValidFT = ensureValidRef(FuncTablePtr, IR_ADDR);
    ir_ref FuncPtr = ir_LOAD_A(ir_ADD_A(
        ValidFT,
        ir_CONST_ADDR(static_cast<uintptr_t>(ResolvedFuncIdx) * sizeof(void *))));

    ir_ref CalleeArgs = SharedCallArgs;
    for (uint32_t i = 0; i < NumArgs; ++i) {
      ir_ref SlotAddr =
          ir_ADD_A(CalleeArgs, ir_CONST_ADDR(i * sizeof(uint64_t)));
      ir_STORE(SlotAddr, WasmArgs[i]);
    }

    // Prototype: ret func(JitExecEnv*, uint64_t* args)
    ir_type DirectRetType;
    if (RetType == IR_FLOAT) {
      DirectRetType = IR_FLOAT;
    } else if (RetType == IR_DOUBLE) {
      DirectRetType = IR_DOUBLE;
    } else {
      DirectRetType = IR_I64;
    }
    uint8_t BufProtoParams[2] = {IR_ADDR, IR_ADDR};
    ir_ref BufProto = ir_proto(ctx, IR_FASTCALL_FUNC, DirectRetType,
                               2, BufProtoParams);
    ir_ref TypedFuncPtr =
        ir_emit2(ctx, IR_OPT(IR_PROTO, IR_ADDR), FuncPtr, BufProto);

    ir_ref BufArgs[2] = {EnvPtrVal, CalleeArgs};
    ir_ref CallResult = ir_CALL_N(DirectRetType, TypedFuncPtr, 2, BufArgs);

    if (!RetTypes.empty()) {
      if (RetType == IR_I32)
        push(ir_TRUNC_I32(CallResult));
      else
        push(CallResult);
    }
  } else {
    // Direct call with ≤kRegCallMaxParams: register-based ABI.
    // Pass each wasm arg in its native type register.
    ir_ref ValidFT = ensureValidRef(FuncTablePtr, IR_ADDR);
    ir_ref FuncPtr = ir_LOAD_A(ir_ADD_A(
        ValidFT,
        ir_CONST_ADDR(static_cast<uintptr_t>(ResolvedFuncIdx) * sizeof(void *))));

    // Build per-callee-type prototype: ret func(JitExecEnv*, arg0, arg1, ...)
    // Always declare I64 return for void/integer callees (IR_VOID triggers an
    // assertion in the IR x86 backend's address-fusion pass at O2).
    ir_type DirectRetType;
    if (RetType == IR_FLOAT) {
      DirectRetType = IR_FLOAT;
    } else if (RetType == IR_DOUBLE) {
      DirectRetType = IR_DOUBLE;
    } else {
      DirectRetType = IR_I64;
    }

    uint32_t TotalParams = 1 + NumArgs; // EnvPtr + wasm args
    std::vector<uint8_t> ProtoParams(TotalParams);
    ProtoParams[0] = IR_ADDR; // JitExecEnv*
    for (uint32_t i = 0; i < NumArgs; ++i) {
      ProtoParams[i + 1] = static_cast<uint8_t>(wasmTypeToIRType(ParamTypes[i]));
    }
    ir_ref DirectProto = ir_proto(ctx, IR_FASTCALL_FUNC, DirectRetType,
                                  TotalParams, ProtoParams.data());
    ir_ref TypedFuncPtr =
        ir_emit2(ctx, IR_OPT(IR_PROTO, IR_ADDR), FuncPtr, DirectProto);

    // Pass args directly in registers: EnvPtr, then each wasm arg
    std::vector<ir_ref> DirectArgs(TotalParams);
    DirectArgs[0] = EnvPtrVal;
    for (uint32_t i = 0; i < NumArgs; ++i) {
      DirectArgs[i + 1] = WasmArgs[i];
    }
    ir_ref CallResult = ir_CALL_N(DirectRetType, TypedFuncPtr,
                                   TotalParams, DirectArgs.data());

    if (!RetTypes.empty()) {
      if (RetType == IR_I32)
        push(ir_TRUNC_I32(CallResult));
      else
        push(CallResult);
    }
  }

  return {};
}

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT

