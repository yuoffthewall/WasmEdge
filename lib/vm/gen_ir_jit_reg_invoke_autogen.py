#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Generates ir_jit_reg_invoke_autogen.cpp — SysV dispatch for arity 0..6, I32/I64/F32/F64 params.

from itertools import product

T = ("I32", "I64", "F32", "F64")


def slot_expr(i: int, kind: str) -> str:
    if kind == "I32":
        return (
            "static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(Slots["
            + str(i)
            + "])))"
        )
    if kind == "I64":
        return "Slots[" + str(i) + "]"
    if kind == "F32":
        return "bitCastF32(Slots[" + str(i) + "])"
    if kind == "F64":
        return "bitCastF64(Slots[" + str(i) + "])"
    raise RuntimeError(kind)


def cpp_param_type(kind: str) -> str:
    return {
        "I32": "int64_t",
        "I64": "uint64_t",
        "F32": "float",
        "F64": "double",
    }[kind]


def cast_inner(types: tuple[str, ...]) -> str:
    parts = ["JitExecEnv *"] + [cpp_param_type(k) for k in types]
    return ", ".join(parts)


def call_args(types: tuple[str, ...]) -> str:
    s = "Env"
    for i, k in enumerate(types):
        s += ", " + slot_expr(i, k)
    return s


def build_call_and_return(ret: str, types: tuple[str, ...]) -> str:
    ci = cast_inner(types)
    ca = call_args(types)
    if ret == "I32":
        if not types:
            return (
                "      {\n"
                "        uint64_t R = reinterpret_cast<uint64_t (*)("
                + ci
                + ")>(Fn)("
                + ca
                + ");\n"
                "        return static_cast<uint64_t>(static_cast<uint32_t>(R));\n"
                "      }\n"
            )
        return (
            "      {\n"
            "        uint64_t R = reinterpret_cast<uint64_t (*)("
            + ci
            + ")>(Fn)("
            + ca
            + ");\n"
            "        return static_cast<uint64_t>(static_cast<uint32_t>(R));\n"
            "      }\n"
        )
    if ret == "I64":
        return (
            "      return reinterpret_cast<uint64_t (*)("
            + ci
            + ")>(Fn)("
            + ca
            + ");\n"
        )
    if ret == "F32":
        return (
            "      {\n"
            "        float R = reinterpret_cast<float (*)("
            + ci
            + ")>(Fn)("
            + ca
            + ");\n"
            "        uint64_t U = 0;\n"
            "        std::memcpy(&U, &R, sizeof(R));\n"
            "        return U;\n"
            "      }\n"
        )
    if ret == "F64":
        return (
            "      {\n"
            "        double R = reinterpret_cast<double (*)("
            + ci
            + ")>(Fn)("
            + ca
            + ");\n"
            "        uint64_t U = 0;\n"
            "        std::memcpy(&U, &R, sizeof(R));\n"
            "        return U;\n"
            "      }\n"
        )
    raise RuntimeError(ret)


def emit_autogen_value_returns() -> str:
    out: list[str] = []
    out.append(
        "uint64_t irJitInvokeNativeAutogen(void *Fn, JitExecEnv *Env,\n"
        "                                  Span<const ValType> ParamTypes,\n"
        "                                  const uint64_t *Slots,\n"
        "                                  ValType RetType) {\n"
        "  switch (RetType.getCode()) {\n"
    )
    for ret in T:
        out.append("  case TypeCode::" + ret + ": {\n")
        out.append("    switch (ParamTypes.size()) {\n")
        for n in range(0, 7):
            out.append("    case " + str(n) + ": {\n")
            if n == 0:
                out.append(build_call_and_return(ret, tuple()))
                out.append("    }\n")
                continue
            combos = list(product(T, repeat=n))
            for combo in combos:
                conds = " && ".join(
                    [
                        "ParamTypes[" + str(i) + "].getCode() == TypeCode::" + combo[i]
                        for i in range(n)
                    ]
                )
                out.append("      if (" + conds + ") {\n")
                body = build_call_and_return(ret, combo)
                for line in body.strip().splitlines():
                    out.append(line + "\n")
                out.append("      }\n")
            out.append("      std::abort();\n")
            out.append("    }\n")
        out.append("    default:\n")
        out.append("      std::abort();\n")
        out.append("    }\n")
        out.append("  }\n")
    out.append("  default:\n")
    out.append("    std::abort();\n")
    out.append("  }\n")
    out.append("}\n\n")
    return "".join(out)


def build_void_call(types: tuple[str, ...]) -> str:
    params = ", ".join([cpp_param_type(k) for k in types])
    inner = "void (*)(JitExecEnv *" + (", " + params if params else "") + ")"
    return "      reinterpret_cast<" + inner + ">(Fn)(" + call_args(types) + ");\n"


def emit_autogen_void() -> str:
    out: list[str] = []
    out.append(
        "void irJitInvokeNativeAutogenVoid(void *Fn, JitExecEnv *Env,\n"
        "                                  Span<const ValType> ParamTypes,\n"
        "                                  const uint64_t *Slots) {\n"
        "  switch (ParamTypes.size()) {\n"
    )
    for n in range(0, 7):
        out.append("  case " + str(n) + ": {\n")
        if n == 0:
            out.append("    reinterpret_cast<void (*)(JitExecEnv *)>(Fn)(Env);\n")
            out.append("    return;\n")
            out.append("  }\n")
            continue
        combos = list(product(T, repeat=n))
        for combo in combos:
            conds = " && ".join(
                [
                    "ParamTypes[" + str(i) + "].getCode() == TypeCode::" + combo[i]
                    for i in range(n)
                ]
            )
            out.append("    if (" + conds + ") {\n")
            out.append(build_void_call(combo))
            out.append("      return;\n")
            out.append("    }\n")
        out.append("    std::abort();\n")
        out.append("  }\n")
    out.append("  default:\n")
    out.append("    std::abort();\n")
    out.append("  }\n")
    out.append("}\n")
    return "".join(out)


def main() -> None:
    header = """// SPDX-License-Identifier: Apache-2.0
// AUTO-GENERATED by lib/vm/gen_ir_jit_reg_invoke_autogen.py — do not edit by hand.

#include "vm/ir_jit_reg_invoke.h"

#ifdef WASMEDGE_BUILD_IR_JIT

#include "ast/type.h"
#include "common/span.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace WasmEdge::VM::detail {

static float bitCastF32(uint64_t U) {
  float F;
  std::memcpy(&F, &U, sizeof(F));
  return F;
}
static double bitCastF64(uint64_t U) {
  double D;
  std::memcpy(&D, &U, sizeof(D));
  return D;
}

"""
    footer = """} // namespace WasmEdge::VM::detail

#endif // WASMEDGE_BUILD_IR_JIT
"""
    body = emit_autogen_value_returns() + emit_autogen_void()
    path = __file__.replace("gen_ir_jit_reg_invoke_autogen.py", "ir_jit_reg_invoke_autogen.cpp")
    with open(path, "w", encoding="utf-8") as f:
        f.write(header + body + footer)
    print("Wrote", path)


if __name__ == "__main__":
    main()
