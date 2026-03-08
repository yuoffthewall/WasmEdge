// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC

#include "vm/ir_builder.h"

#ifdef WASMEDGE_BUILD_IR_JIT

#include "ast/instruction.h"
#include "ast/type.h"
#include "common/errcode.h"
#include "vm/ir_jit_engine.h"

// Include dstogov/ir headers
extern "C" {
#include "ir.h"
#include "ir_builder.h"
}

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>

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
}  // namespace

WasmToIRBuilder::WasmToIRBuilder() noexcept
    : Initialized(false), CurrentPathTerminated(false), EnvPtr(0), FuncTablePtr(0), FuncTableSize(0), GlobalBasePtr(0), MemoryBase(0), ArgsPtr(0), MemorySize(0), LocalCount(0) {}

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
  ArgsPtr = 0;
  MemorySize = 0;
  LocalCount = 0;
  ModuleFuncTypes.clear();
  ModuleGlobalTypes.clear();
}

Expect<void> WasmToIRBuilder::initialize(
    const AST::FunctionType &FuncType,
    Span<const std::pair<uint32_t, ValType>> LocalVars) {
  reset();

  // Initialize IR context. Default O2; override with WASMEDGE_IR_JIT_OPT_LEVEL=0|1 for debug.
  int ir_opt_level = 2;
  if (const char *e = std::getenv("WASMEDGE_IR_JIT_OPT_LEVEL")) {
    if (e[0] == '0' && e[1] == '\0') ir_opt_level = 0;
    else if (e[0] == '1' && e[1] == '\0') ir_opt_level = 1;
  }
  uint32_t ir_flags = IR_FUNCTION;
  if (ir_opt_level > 0) {
    ir_flags |= IR_OPT_FOLDING | IR_OPT_CFG | IR_OPT_CODEGEN;
    if (ir_opt_level > 1) ir_flags |= IR_OPT_MEM2SSA | IR_OPT_INLINE;
  }
  ir_init(&Ctx, ir_flags, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
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
  
  // Calculate total locals
  LocalCount = ParamTypes.size();
  for (const auto &[Count, Type] : LocalVars) {
    LocalCount += Count;
  }

  // Uniform JIT signature: ret func(JitExecEnv* env, uint64_t* args)
  // O2 emitter required; O0 fuses LOAD(addr)->ADDR with ADD incorrectly.
  EnvPtr = ir_PARAM(IR_ADDR, "exec_env", 1);
  ArgsPtr = ir_PARAM(IR_ADDR, "args", 2);

  FuncTablePtr = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, FuncTable))));
  FuncTableSize = ir_LOAD_U32(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, FuncTableSize))));
  GlobalBasePtr = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, GlobalBase))));
  MemoryBase = ir_LOAD_A(ir_ADD_A(EnvPtr, ir_CONST_ADDR(offsetof(JitExecEnv, MemoryBase))));

  // Load wasm parameters from the args array. Each slot is sizeof(uint64_t).
  // On little-endian (x86_64), loading a narrower type from the slot address
  // reads the correct lower bytes.
  for (uint32_t i = 0; i < ParamTypes.size(); ++i) {
    ir_type irType = wasmTypeToIRType(ParamTypes[i]);
    ir_ref SlotAddr = ir_ADD_A(ArgsPtr, ir_CONST_ADDR(i * sizeof(uint64_t)));
    Locals[i] = ir_LOAD(irType, SlotAddr);
    LocalTypes[i] = irType;
  }

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

  // MemorySize is not passed as parameter for now
  // Bounds checking would require additional parameter or global
  MemorySize = IR_UNUSED;

  return {};
}

Expect<void>
WasmToIRBuilder::buildFromInstructions(Span<const AST::Instruction> Instrs) {
  for (const auto &Instr : Instrs) {
    auto Res = visitInstruction(Instr);
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
  case OpCode::F64__add:
  case OpCode::F64__sub:
  case OpCode::F64__mul:
  case OpCode::F64__div:
  case OpCode::F64__min:
  case OpCode::F64__max:
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

  // Bulk memory operations
  case OpCode::Memory__copy: {
    // memory.copy: dst, src, n -> copies n bytes from src to dst
    ir_ctx *ctx = &Ctx;
    ir_ref N = pop();      // Number of bytes
    ir_ref Src = pop();    // Source address (i32)
    ir_ref Dst = pop();    // Destination address (i32)
    
    // Convert to native addresses
    ir_ref SrcAddr = ir_ADD_A(MemoryBase, ir_ZEXT_A(Src));
    ir_ref DstAddr = ir_ADD_A(MemoryBase, ir_ZEXT_A(Dst));
    ir_ref Size = ir_ZEXT_A(N);  // Size as native address type for memmove
    
    // Call memmove (handles overlapping regions correctly)
    // void *memmove(void *dest, const void *src, size_t n);
    ir_ref Proto = ir_proto_3(ctx, IR_FASTCALL_FUNC, IR_ADDR, IR_ADDR, IR_ADDR, IR_ADDR);
    ir_ref MemmoveFunc = ir_const_func_addr(ctx, (uintptr_t)&memmove, Proto);
    ir_CALL_3(IR_ADDR, MemmoveFunc, DstAddr, SrcAddr, Size);
    return {};
  }

  case OpCode::Memory__fill: {
    // memory.fill: dst, val, n -> fills n bytes at dst with val
    ir_ctx *ctx = &Ctx;
    ir_ref N = pop();      // Number of bytes
    ir_ref Val = pop();    // Value to fill (i32, only low byte used)
    ir_ref Dst = pop();    // Destination address (i32)
    
    // Convert to native address
    ir_ref DstAddr = ir_ADD_A(MemoryBase, ir_ZEXT_A(Dst));
    ir_ref Size = ir_ZEXT_A(N);
    
    // Call memset
    // void *memset(void *s, int c, size_t n);
    ir_ref Proto = ir_proto_3(ctx, IR_FASTCALL_FUNC, IR_ADDR, IR_ADDR, IR_I32, IR_ADDR);
    ir_ref MemsetFunc = ir_const_func_addr(ctx, (uintptr_t)&memset, Proto);
    ir_CALL_3(IR_ADDR, MemsetFunc, DstAddr, Val, Size);
    return {};
  }
  
  case OpCode::Memory__size: {
    // memory.size: returns the current memory size in pages
    // For POC: return 0 (would need memory instance to get real size)
    ir_ctx *ctx = &Ctx;
    ir_ref Size = ir_CONST_I32(0);  // Placeholder - need runtime access
    push(Size);
    return {};
  }
  
  case OpCode::Memory__grow: {
    // memory.grow: tries to grow memory by n pages, returns old size or -1
    // For POC: always return -1 (growth failed)
    ir_ctx *ctx = &Ctx;
    (void)pop();  // Discard the growth amount
    ir_ref Result = ir_CONST_I32(-1);  // Placeholder - always fail
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
  case OpCode::I32__popcnt:
    Result = ir_CTPOP_I32(Operand);
    break;

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
  case OpCode::I64__popcnt:
    Result = ir_CTPOP_I64(Operand);
    break;

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

ir_ref WasmToIRBuilder::buildBoundsCheck(ir_ref Address, uint32_t AccessSize) {
  // Simplified: skip bounds checking for POC
  // In production, would add runtime checks here
  (void)Address;
  (void)AccessSize;
  return IR_TRUE;
}

Expect<void> WasmToIRBuilder::visitMemory(const AST::Instruction &Instr) {
  OpCode Op = Instr.getOpCode();
  uint32_t Offset = Instr.getMemoryOffset();
  (void)Instr.getMemoryAlign(); // Alignment hints not enforced in POC

  ir_ctx *ctx = &Ctx; // For IR macros
  ir_ref Result = IR_UNUSED;

  // Check if this is a load or store operation
  bool IsLoad = (Op >= OpCode::I32__load && Op <= OpCode::I64__load32_u);
  
  if (IsLoad) {
    // Load operations: pop address, load from memory, push result
    ir_ref BaseAddr = pop(); // Address from stack (i32)
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

Expect<void> WasmToIRBuilder::visitControl(const AST::Instruction &Instr) {
  OpCode Op = Instr.getOpCode();

  switch (Op) {
  case OpCode::Nop:
    // Nop does nothing
    return {};
  case OpCode::Unreachable: {
    // Unreachable - generate trap/abort
    ir_ctx *ctx = &Ctx;
    ir_UNREACHABLE();
    // Mark current path as terminated
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

std::map<uint32_t, ir_ref> WasmToIRBuilder::mergeLocals(
    const std::vector<std::map<uint32_t, ir_ref>> &EndLocals) {
  ir_ctx *ctx = &Ctx;
  size_t NumPaths = EndLocals.size();

  std::set<uint32_t> AllLocalIndices;
  for (const auto &PathLocals : EndLocals) {
    for (const auto &[Idx, _] : PathLocals) {
      AllLocalIndices.insert(Idx);
    }
  }

  std::map<uint32_t, ir_ref> Merged;
  for (uint32_t LocalIdx : AllLocalIndices) {
    std::vector<ir_ref> Values;
    bool AllSame = true;
    ir_ref FirstVal = IR_UNUSED;
    for (const auto &PathLocals : EndLocals) {
      auto it = PathLocals.find(LocalIdx);
      if (it != PathLocals.end()) {
        if (FirstVal == IR_UNUSED) {
          FirstVal = it->second;
        } else if (it->second != FirstVal) {
          AllSame = false;
        }
        Values.push_back(it->second);
      } else if (FirstVal != IR_UNUSED) {
        // Local missing on this path -- reuse the first path's value so the
        // PHI input count matches the MERGE input count.
        Values.push_back(FirstVal);
      }
    }
    if (Values.size() != NumPaths) {
      continue;
    }
    if (AllSame) {
      Merged[LocalIdx] = FirstVal;
    } else {
      ir_type LocalType = IR_I32;
      auto typeIt = LocalTypes.find(LocalIdx);
      if (typeIt != LocalTypes.end()) {
        LocalType = typeIt->second;
      }
      std::vector<ir_ref> Coerced;
      for (ir_ref V : Values) {
        Coerced.push_back(coerceToType(V, LocalType));
      }
      if (NumPaths == 2) {
        ir_ref Phi = ir_PHI_2(LocalType, Coerced[0], Coerced[1]);
        Merged[LocalIdx] = Phi;
      } else {
        ir_ref Phi = ir_PHI_N(LocalType,
                               static_cast<ir_ref>(Coerced.size()),
                               Coerced.data());
        Merged[LocalIdx] = Phi;
      }
    }
  }
  return Merged;
}

void WasmToIRBuilder::mergeResults(const std::vector<ir_ref> &BranchResults,
                                   ir_type ResultType) {
  ir_ctx *ctx = &Ctx;
  if (BranchResults.size() == 1) {
    push(BranchResults[0]);
  } else if (BranchResults.size() == 2) {
    ir_ref C0 = coerceToType(BranchResults[0], ResultType);
    ir_ref C1 = coerceToType(BranchResults[1], ResultType);
    push(ir_PHI_2(ResultType, C0, C1));
  } else {
    std::vector<ir_ref> Coerced;
    for (ir_ref R : BranchResults) {
      Coerced.push_back(coerceToType(R, ResultType));
    }
    push(ir_PHI_N(ResultType, static_cast<ir_ref>(Coerced.size()),
                  Coerced.data()));
  }
}

void WasmToIRBuilder::emitLoopBackEdge(LabelInfo &Target) {
  ir_ctx *ctx = &Ctx;

  for (auto &[LocalIdx, PhiRef] : Target.LoopLocalPhis) {
    auto it = Locals.find(LocalIdx);
    if (it != Locals.end()) {
      ir_type PhiType = IR_I32;
      auto typeIt = LocalTypes.find(LocalIdx);
      if (typeIt != LocalTypes.end()) {
        PhiType = typeIt->second;
      }
      ir_ref CoercedVal = coerceToType(it->second, PhiType);
      ir_PHI_SET_OP(PhiRef, 2, CoercedVal);
    }
  }

  ir_ref LoopEnd = ir_LOOP_END();
  ir_MERGE_SET_OP(Target.LoopHeader, 2, LoopEnd);
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
  } else {
    Label.Arity = 0;
    Label.ResultType = IR_VOID;
  }

  Label.StackBase = static_cast<uint32_t>(ValueStack.size());
  Label.InElseBranch = false;
  Label.HasElse = false;
  Label.TrueBranchTerminated = false;
  Label.ElseBranchTerminated = false;

  // Create PHI nodes for all local variables
  // In SSA form, loop variables need PHI nodes to merge:
  //   - Initial value (before loop)
  //   - Back-edge value (after loop iteration)
  for (auto &[LocalIdx, LocalRef] : Locals) {
    // Use tracked local type (definitive) instead of inferring from value
    ir_type LocalType = IR_I32;  // Default
    auto typeIt = LocalTypes.find(LocalIdx);
    if (typeIt != LocalTypes.end()) {
      LocalType = typeIt->second;
    } else {
      // Local not in LocalTypes - infer from operand
      if (LocalRef > 0 && LocalRef < static_cast<ir_ref>(Ctx.insns_count)) {
        LocalType = static_cast<ir_type>(Ctx.ir_base[LocalRef].type);
      }
    }
    // Create PHI_2: first operand is pre-loop value, second will be set at back-edge
    // Coerce the operand to ensure type matches (handles all int/float conversions)
    ir_ref CoercedRef = coerceToType(LocalRef, LocalType);
    ir_ref Phi = ir_PHI_2(LocalType, CoercedRef, IR_UNUSED);
    
    // Create a COPY of the PHI to materialize its value at the start of each iteration.
    // This ensures we have a concrete SSA value to use when exiting the loop early.
    ir_ref PhiCopy = ir_COPY(LocalType, Phi);
    
    // Store mapping and original value
    Label.PreLoopLocals[LocalIdx] = LocalRef;
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
    ir_ref TrueResult = pop();  // Pop the result value
    Label.BranchResults.push_back(TrueResult);
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
      ir_ref CurrentEnd = ir_END();
      Label.EndList.push_back(CurrentEnd);
      Label.EndLocals.push_back(Locals);  // Save current Locals
    }
    
    // Merge all collected ends
    if (Label.EndList.size() == 1) {
      // Single path, just continue with BEGIN and restore its Locals
      ir_BEGIN(Label.EndList[0]);
      if (!Label.EndLocals.empty()) {
        Locals = Label.EndLocals[0];
      }
      CurrentPathTerminated = false;
    } else if (Label.EndList.size() == 2) {
      ir_MERGE_2(Label.EndList[0], Label.EndList[1]);
      if (Label.EndLocals.size() >= 2) {
        Locals = mergeLocals(Label.EndLocals);
      } else if (!Label.EndLocals.empty()) {
        Locals = Label.EndLocals[0];
      }
      CurrentPathTerminated = false;
    } else if (Label.EndList.size() > 2) {
      ir_MERGE_N(static_cast<ir_ref>(Label.EndList.size()), Label.EndList.data());
      if (!Label.EndLocals.empty()) {
        Locals = mergeLocals(Label.EndLocals);
      }
      CurrentPathTerminated = false;
    }
    // else: no paths to merge (all terminated), CurrentPathTerminated stays true
    break;
  }
  
  case ControlKind::Loop: {
    // Loop end: when falling through (exiting the loop normally),
    // keep the current Locals values. They were updated by local.set
    // instructions inside the loop and represent the final state.
    // Do NOT restore pre-loop values - wasm semantics require locals
    // to retain their modified values after exiting a loop.
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
      ir_ref BranchResult = pop();  // Pop the result value
      Label.BranchResults.push_back(BranchResult);
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
        // Need a default value for the "no else" case
        // This shouldn't happen in well-formed Wasm with result types
        // but provide a default just in case
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
      ir_ref FalseEnd = ir_END();
      Label.EndList.push_back(FalseEnd);
    }
    
    // Merge the branches (only if we have paths to merge)
    if (Label.EndList.size() == 1) {
      // Single path continuing
      ir_BEGIN(Label.EndList[0]);
      CurrentPathTerminated = false;
      // Use that path's locals directly
      if (!Label.EndLocals.empty()) {
        Locals = Label.EndLocals[0];
      }
      // Push single result value
      if (Label.Arity > 0 && !Label.BranchResults.empty()) {
        push(Label.BranchResults[0]);
      }
    } else if (Label.EndList.size() >= 2) {
      if (Label.EndList.size() == 2) {
        ir_MERGE_2(Label.EndList[0], Label.EndList[1]);
      } else {
        ir_MERGE_N(static_cast<ir_ref>(Label.EndList.size()), Label.EndList.data());
      }
      CurrentPathTerminated = false;
      if (!Label.EndLocals.empty()) {
        Locals = mergeLocals(Label.EndLocals);
      }
      if (Label.Arity > 0 && Label.BranchResults.size() == Label.EndList.size()) {
        mergeResults(Label.BranchResults, Label.ResultType);
      }
    }
    // else: no live paths (shouldn't happen if we handled both-terminated case above)
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
  
  // Implementation using chained if-else (simpler than ir_SWITCH for Wasm semantics)
  // This generates: if (idx == 0) br label0; else if (idx == 1) br label1; ... else br default;
  
  for (uint32_t i = 0; i < NumCases; i++) {
    uint32_t LabelIdx = Labels[i].TargetIndex;
    
    if (LabelIdx >= LabelStack.size()) {
      return Unexpect(ErrCode::Value::RuntimeError);
    }
    
    LabelInfo &Target = LabelStack[LabelStack.size() - 1 - LabelIdx];
    
    // Compare: idx == i
    ir_ref CaseVal = ir_CONST_I32(static_cast<int32_t>(i));
    ir_ref Cmp = ir_EQ(IndexVal, CaseVal);
    
    // if (idx == i)
    ir_ref IfRef = ir_IF(Cmp);
    
    // True branch: branch to target
    ir_IF_TRUE(IfRef);
    
    if (Target.Kind == ControlKind::Loop) {
      emitLoopBackEdge(Target);
    } else {
      ir_ref BrEnd = ir_END();
      Target.EndList.push_back(BrEnd);
      Target.EndLocals.push_back(Locals);
    }
    
    ir_IF_FALSE(IfRef);
  }
  
  // Default case (all comparisons failed, or out of bounds)
  LabelInfo &DefaultTarget = LabelStack[LabelStack.size() - 1 - DefaultLabelIdx];
  
  if (DefaultTarget.Kind == ControlKind::Loop) {
    emitLoopBackEdge(DefaultTarget);
  } else {
    ir_ref BrEnd = ir_END();
    DefaultTarget.EndList.push_back(BrEnd);
    DefaultTarget.EndLocals.push_back(Locals);
  }
  
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
  ir_ref FuncPtr = IR_UNUSED;

  if (Op == OpCode::Call_indirect) {
    uint32_t TypeIdx = Instr.getTargetIndex();
    if (TypeIdx >= ModuleFuncTypes.size())
      return Unexpect(ErrCode::Value::RuntimeError);
    TargetFuncType = ModuleFuncTypes[TypeIdx];
  } else {
    uint32_t FuncIdx = Instr.getTargetIndex();
    if (FuncIdx >= ModuleFuncTypes.size())
      return Unexpect(ErrCode::Value::RuntimeError);
    TargetFuncType = ModuleFuncTypes[FuncIdx];
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

  // Load function pointer from the function table.
  ir_ref ValidFT = ensureValidRef(FuncTablePtr, IR_ADDR);
  if (Op == OpCode::Call_indirect) {
    ir_ref Idx = ir_ZEXT_A(TableIndex);
    ir_ref Off = ir_MUL_A(Idx, ir_CONST_ADDR(sizeof(void *)));
    FuncPtr = ir_LOAD_A(ir_ADD_A(ValidFT, Off));
  } else {
    uint32_t FuncIdx = Instr.getTargetIndex();
    ir_ref Off = ir_MUL_A(ir_CONST_ADDR(static_cast<uintptr_t>(FuncIdx)),
                          ir_CONST_ADDR(sizeof(void *)));
    FuncPtr = ir_LOAD_A(ir_ADD_A(ValidFT, Off));
  }

  // Allocate callee args array on the stack: uint64_t args[N].
  uint32_t NumArgs = static_cast<uint32_t>(ParamTypes.size());
  ir_ref CalleeArgs = IR_UNUSED;
  if (NumArgs > 0) {
    CalleeArgs = ir_ALLOCA(ir_CONST_I32(NumArgs * sizeof(uint64_t)));
    for (uint32_t i = 0; i < NumArgs; ++i) {
      ir_ref SlotAddr =
          ir_ADD_A(CalleeArgs, ir_CONST_ADDR(i * sizeof(uint64_t)));
      ir_STORE(SlotAddr, WasmArgs[i]);
    }
  } else {
    CalleeArgs = ir_CONST_ADDR(0);
  }

  // Determine return type.
  ir_type RetType = IR_VOID;
  if (!RetTypes.empty())
    RetType = wasmTypeToIRType(RetTypes[0]);

  // Build prototype: ret func(JitExecEnv* env, uint64_t* args)
  uint8_t ProtoParams[2] = {IR_ADDR, IR_ADDR};
  ir_ref Proto =
      ir_proto(ctx, IR_FASTCALL_FUNC, RetType, 2, ProtoParams);
  ir_ref TypedFuncPtr =
      ir_emit2(ctx, IR_OPT(IR_PROTO, IR_ADDR), FuncPtr, Proto);

  ir_ref CallArgs[2] = {ensureValidRef(EnvPtr, IR_ADDR), CalleeArgs};
  ir_ref CallResult =
      ir_CALL_N(RetType, TypedFuncPtr, 2, CallArgs);

  // Free the stack allocation.
  if (NumArgs > 0)
    ir_AFREE(ir_CONST_I32(NumArgs * sizeof(uint64_t)));

  if (!RetTypes.empty())
    push(CallResult);

  return {};
}

} // namespace VM
} // namespace WasmEdge

#endif // WASMEDGE_BUILD_IR_JIT

