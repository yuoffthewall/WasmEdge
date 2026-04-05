# Tier-2 Performance Improvement Analysis

**Date:** 2026-04-05
**Branch:** `tier2`

---

## Summary

Analyzed the tier-2 pipeline for high-impact runtime performance opportunities.
The single dominant bottleneck is the per-function compilation model that
prevents LLVM from performing cross-function optimization.

---

## #1: Cross-Function Inlining via Multi-Function LLVM Modules (HIGH IMPACT)

**Status:** Recommended — this is the big win.

### The Problem

Every tier-2 function is compiled in its own LLJIT instance
(`tier2_compiler.cpp:332`). Each becomes a standalone LLVM module with one
function definition and `declare` stubs for external helpers. LLVM cannot
inline any wasm-to-wasm calls.

Every direct call currently does:

1. **Caller:** store all args to stack-allocated `uint64_t[]` buffer
   (`ir_builder.cpp:2947-2955`)
2. **Caller:** indirect call through `FuncTable[idx]` — load + indirect branch
   (`ir_builder.cpp:3064-3090`)
3. **Callee:** load all args back from the buffer

LLVM's inliner, SROA, and interprocedural optimizations are completely
neutered because each module has exactly one function.

### The Fix

Batch hot functions into a **single LLVM module** before running LLVM
optimization passes. When a function tiers up, don't compile it immediately —
collect it with its hot callees, emit them all into one LLVM module, let LLVM
inline across them, then codegen the whole batch.

- FuncTable swap still happens per-function (each function has its own symbol)
- Need batching logic in `Tier2Manager` (collect call graph, wait for callees)
- Need multi-function `ir_emit_llvm` emission (or concatenate per-function
  LLVM IR text into one module)
- Single LLJIT per batch instead of per function

### Expected Impact

For workloads with hot call chains (almost all real code), inlining eliminates
arg marshaling overhead entirely, enables constant propagation across call
boundaries, and unlocks LLVM's full optimization suite. Depending on call
density: **2-5x for call-heavy hot paths**.

### Difficulty

Medium-high. Batching logic, call graph extraction, multi-function emission.

---

## #2: Register-Based Calling Convention for Tier-2 (DROPPED)

**Status:** Not worth pursuing as a standalone optimization.

### Background

Register-based calling was implemented on branch `reg_call` and reverted
(see `notes/reg_based_call_doc.md`). The root cause: dstogov/ir's LSRA pins
`ir_PARAM` values to callee-saved registers for their full live range, causing
spill pressure that outweighs the benefit of avoiding buffer marshaling.

### Why It Doesn't Apply to Tier-2 (LLVM)

The `ir_PARAM` register pressure problem is specific to dstogov/ir's allocator.
LLVM's greedy register allocator splits live ranges, rematerializes, and freely
assigns caller-saved registers to short-lived params. So the "register-based
hurts" finding does **not** carry over to tier-2 LLVM code.

### Why It's Still Not Worth Doing

The architectural problem remains: every wasm direct call goes through
`FuncTable[idx]` — an indirect dispatch. The caller doesn't know whether the
callee is tier-1 or tier-2. Changing the ABI requires:

- Tier-1 caller → tier-2 callee: still needs buffer ABI
- Tier-2 caller → tier-1 callee: still needs buffer ABI
- Tier-2 caller → tier-2 callee: could use register ABI, but tier-up is
  asynchronous — you don't know at compile time if the callee is tier-2

A dual-entry-point scheme (buffer wrapper + native-ABI inner function) would
add significant complexity for uncertain payoff on indirect calls that LLVM
can't devirtualize anyway.

### Why #1 Makes #2 Free

If hot functions are in the same LLVM module (#1), LLVM inlines hot callees.
Inlined calls have no calling convention — no arg marshaling, no indirect
dispatch, no register pressure from params. For non-inlined calls within the
same module, LLVM uses internal linkage and its own optimized convention
automatically.

---

## #3: MemoryBase / Env Field Hoisting (MODERATE)

**Status:** Worth investigating, low effort.

### The Problem

`MemoryBase` is loaded from `JitExecEnv` at function entry
(`ir_builder.cpp:345`). In tier-2 LLVM code, it's loaded through an opaque
`env` pointer. LLVM may not prove it's loop-invariant, re-loading it every
iteration in memory-access-heavy loops.

Same applies to `FuncTablePtr`, `GlobalBasePtr`, and other env fields that
don't change within a function (except after `memory.grow`).

### The Fix

In the LLVM IR emitted by `ir_emit_llvm`, annotate env-field loads with
`!invariant.load` metadata, or mark the env pointer with appropriate `readonly`
/ `noalias` attributes. This lets LLVM hoist them out of loops.

### Expected Impact

**10-20%** on memory-access-heavy loops. Easy to verify by dumping tier-2
LLVM IR (`WASMEDGE_TIER2_DUMP_IR=1`) and checking if MemoryBase loads are
hoisted.

### Difficulty

Low. Metadata annotation in `ir_emit_llvm.c` or post-processing the LLVM IR.

---

## Compile-Time Optimizations (LOW PRIORITY)

These reduce how long it takes to compile a function on the background thread.
Since compilation is async on `SCHED_IDLE`, they don't affect steady-state
runtime performance. Only relevant if faster warmup matters.

### Text Serialization Round-Trips

The pipeline does: `ir_save()` → IR text → `ir_load_safe()` → fresh `ir_ctx`
→ GCM passes → `ir_emit_llvm()` → LLVM IR text → `LLVMParseIRInContext()`.
Three serialize/deserialize cycles. A binary IR format or direct `ir_ctx`
handoff would cut compile latency.

### GCM Passes Run Twice

`ir_build_cfg`, `ir_build_dominators_tree`, `ir_find_loops`, `ir_gcm`,
`ir_schedule`, `ir_schedule_blocks` all run in tier-1 and again in tier-2
on the reloaded context. Caching scheduling results or deferring to LLVM's
own passes would help.

### LLJIT Instance Per Function

Each tier-2 function creates a new `LLVMOrcLLJIT` instance (intentionally
leaked). Reusing a single LLJIT with multiple JITDylibs would save
~1-5 MB per function in memory overhead.

---

## Priority Order

1. **#1 Cross-function inlining** — the dominant win, unlocks LLVM's real power
2. **#3 MemoryBase hoisting** — easy, measurable, independent of #1
3. **Compile-time opts** — only if warmup latency becomes a problem
4. ~~#2 Register-based ABI~~ — dropped, subsumed by #1
