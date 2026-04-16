# On-Stack Replacement (OSR) for hot loops

## Problem

Tier-2 uses a function-entry swap mechanism: when a batch is compiled,
`FuncTable[idx]` is updated to point at `fN_fwd_thunk`. Any *future*
call to that function dispatches through the LLVM-compiled thunk.

This does nothing for functions that are already on the call stack.
The majority of sightglass kernels have a single long-running hot loop
inside a one-shot caller (`__original_main` or similar). By the time
the tier-2 batch compiles (~200ms), the caller is mid-loop executing
tier-1 code. The `FuncTable` swap installs `fN_fwd_thunk` for a call
that never happens again.

Execution timeline (current):

```
_start -> __original_main (tier-1, called ONCE)
           |
           +-- byte 1-10:  tier-1 loop body, isalnum via tier-1
           +-- [isalnum trips threshold -> enqueue batch {9,22,23,24,...}]
           +-- [background: LLVM compiles batch -> f9_fwd_thunk ready]
           +-- [FuncTable[9] swapped -> f9_fwd_thunk installed]
           |     ^ but nobody calls func 9 again!
           +-- byte 11+:   tier-1 loop body, isalnum via fwd_thunk
           |                (each call: indirect FuncTable -> thunk)
           +-- byte 49999: still tier-1 loop body
```

This is the binding constraint on the loss-cluster kernels post-P1c:

| Kernel           | tier-1 only | tier-2 (isolated) | LLVM O2   | t2/t1 | t1/LLVM  |
|------------------|------------:|------------------:|----------:|------:|---------:|
| shootout-ctype   |   8,236,909 |         8,115,602 | 5,007,782 | 0.99x | **0.61x**|
| shootout-ed25519 |   8,551,934 |         9,455,472 | 5,230,330 | 1.11x | **0.61x**|
| blind-sig        |   9,536,793 |         9,321,811 | 3,958,780 | 0.98x | **0.42x**|

Tier-2's overhead is ~0% (t2/t1 ~ 1.0). The entire gap to LLVM is
the structural difference between tier-1 and LLVM codegen quality on
the hot loop body. Function-entry swap cannot close it.

---

## Solution: on-stack replacement

OSR replaces the *currently running* function while it is on the call
stack. Instead of waiting for the next function call, the runtime:

1. Detects a hot loop back-edge via a counter at the back-edge itself
2. Compiles a special entry point for the loop body -- not the
   function's normal entry, but a "resume at loop header with these
   live variables" entry
3. At the next back-edge check after compilation completes, transfers
   execution: captures the current wasm locals, jumps into the
   LLVM-compiled loop body

Execution timeline with OSR:

```
_start -> __original_main (tier-1)
           |
           +-- byte 1-10:  tier-1 loop body
           +-- [back-edge counter trips -> enqueue OSR compile]
           +-- [background: LLVM compiles loop entry point]
           +-- byte K:     back-edge check -> OSR entry ready
           |   +-- serialize wasm locals to locals frame
           |   +-- call osr_entry(exec_ctx, local0, local1, ...)
           |   +-- return result (abandon tier-1 execution)
           +-- byte K+1:   LLVM code, isalnum INLINED (zero calls)
           +-- byte 49999: LLVM code
```

### What changes for the loss cluster

|                     | Current (function-entry swap) | With OSR                          |
|---------------------|-------------------------------|-----------------------------------|
| Loop body codegen   | tier-1 for entire run         | tier-1 until OSR, then LLVM       |
| Predicate calls     | indirect via FuncTable        | inlined by LLVM (zero cost)       |
| ctype ceiling       | tier-1 speed (~8.2M us)       | LLVM speed (~5.0M us) + warmup    |
| Applicable to       | functions called multiple times| any hot loop, even in `main`      |

### Theoretical ceiling

Approach LLVM JIT WT minus a small warmup fraction:

| Kernel           | Current (tier-1) | LLVM O2 | Expected with OSR | Improvement |
|------------------|------------------:|--------:|------------------:|------------:|
| shootout-ctype   |            8.2M   |   5.0M  |            ~5.2M  |       ~37%  |
| shootout-ed25519 |            8.5M   |   5.2M  |            ~5.5M  |       ~35%  |
| blind-sig        |            9.5M   |   3.9M  |            ~4.3M  |       ~55%  |

Plus any kernel where the hot loop is in a one-shot caller and tier-1
codegen is measurably worse than LLVM.

---

## How HotSpot and V8 do OSR

Both HotSpot and V8 treat OSR as a first-class mechanism, shipped since
early versions. Understanding their designs clarifies which parts of
OSR are hard in general vs. hard specifically for us.

### HotSpot JVM (C1 + C2)

**Detection.** Every loop back-edge increments a per-method counter in
the interpreter. When the counter overflows, the interpreter calls into
the runtime to request an OSR compilation of the enclosing method.

**Compilation.** C2 compiles the method normally, plus a special "OSR
entry point" that takes the interpreter frame as input. The OSR entry
is a basic block at the target loop header; C2 generates code that
reads locals and stack slots from the interpreter frame (which is a
well-defined, uniform layout -- all values on a virtual stack with
known slots) and feeds them as initial values to the loop's PHI nodes.
The rest of the method is compiled identically to a normal compilation,
including inlining callees based on call-site profile data.

**Transition.** At the next back-edge check (in the interpreter, not in
JIT code -- HotSpot's C1 baseline JIT does NOT do OSR-in from its own
compiled code to C2), the runtime:

1. Reads all locals + operand stack entries from the interpreter frame
2. Passes them to the C2-compiled OSR entry point
3. Execution continues in C2 code from the loop header

**Key advantage: uniform frame layout.** The interpreter's frame is a
flat array of tagged slots at known offsets. Reading local N means
reading `frame[N]`. There is no register allocation to reverse-engineer
-- the interpreter always stores everything in memory. This is why
HotSpot's live variable capture is trivial.

**What we can learn:** HotSpot's OSR-in is always interpreter -> C2,
never C1 -> C2. C1's compiled code has locals in registers (like our
tier-1), and HotSpot chose NOT to do OSR from C1 to C2 -- instead, if
a C1-compiled method has a hot loop, C2 compiles the method from
scratch and the next call to the method enters the C2 version
(function-entry swap, same as our current mechanism). The interpreter
is the only tier that does OSR-in because it's the only tier with a
trivially readable frame layout.

**Implication for us:** We don't have an interpreter tier. Our tier-1
is a register-allocating JIT (dstogov/ir). This puts us in the
position of wanting C1 -> C2 OSR, which HotSpot explicitly avoided.
Our Approach A (serialize locals to memory at every back-edge)
effectively creates a "shadow interpreter frame" that mimics the
interpreter's flat layout, paying the serialization cost that an
interpreter would pay naturally.

### V8 (Sparkplug + Maglev + Turbofan)

**Detection.** Back-edge counters are embedded in baseline-compiled
(Sparkplug) code. Each loop back-edge decrements a per-function
counter; on underflow, calls into the runtime.

**Compilation.** Turbofan compiles the method with an OSR entry.
Turbofan's "OSR value" nodes represent the live state at the loop
header -- they're special IR nodes that load values from the
unoptimized frame at OSR entry. During Turbofan's optimization
pipeline, these OSR value nodes get constant-folded, type-narrowed,
and optimized like any other value.

**Transition.** Similar to HotSpot:

1. At the back-edge counter underflow, the runtime checks if an
   OSR-compiled version is ready (compiled on a concurrent thread)
2. If ready: reads locals from the Sparkplug/Maglev frame, calls the
   Turbofan OSR entry
3. If not ready: triggers a concurrent compile request and continues
   executing in the current tier

**Key difference from HotSpot:** V8 does OSR from Sparkplug (a
non-optimizing register-allocating compiler) into Turbofan. Sparkplug
uses a stack-based frame layout with values at known stack offsets
(it's designed to be trivially deoптimizable). Maglev (the mid-tier)
also has a known frame layout. So V8 solved the "register-allocated
tier -> optimizing tier" OSR problem that HotSpot sidestepped.

**How V8 handles the frame layout problem:** Sparkplug and Maglev are
designed with OSR/deopt in mind from the start -- they maintain a
"virtual frame" in memory (stack slots at known offsets for each
bytecode register) even though they also use machine registers. At
any safepoint (including back-edges), the frame slots are up to date.
This is essentially Approach A (serialize to memory) baked into the
compiler's design rather than bolted on.

**Implication for us:** V8's approach validates Approach A. The cost of
maintaining a shadow frame at back-edges is accepted as the price of
tiering by V8's Sparkplug/Maglev tiers. The difference is that V8
designed the frame layout for OSR from day one, whereas we'd be adding
it to an existing compiler (dstogov/ir) that wasn't designed for it.

### Comparison with our situation

|                      | HotSpot         | V8               | WasmEdge tier-1    |
|----------------------|-----------------|------------------|--------------------|
| OSR source tier      | Interpreter     | Sparkplug/Maglev | dstogov/ir JIT     |
| Frame layout         | Flat slot array | Known stack slots| SSA registers+spills|
| Locals readable at back-edge? | Yes (always in memory) | Yes (shadow frame) | **No** (pure SSA) |
| OSR entry mechanism  | Special C2 entry| Turbofan OSR values | To be built |
| Back-edge counter    | Interpreter counter | Sparkplug counter | **Does not exist** |

**The core lesson:** Both HotSpot and V8 ensure that the OSR source
tier keeps locals in a readable memory location at back-edges. HotSpot
gets this for free (interpreter). V8 pays for it explicitly (shadow
frame in Sparkplug/Maglev). We need to add it to dstogov/ir
(Approach A). This is the proven approach -- there is no production JIT
that does OSR by reverse-engineering register allocator output at
runtime.

---

## Feasibility evaluation (2026-04-16)

Grounded in code inspection of `ir_builder.cpp`, `tier2_compiler.cpp`,
`tier2_manager.cpp`, `ir_jit_engine.h`, and `thirdparty/ir/`.

### Sub-problem 1: back-edge detection

**Current state: no back-edge counter exists.**

`WASMEDGE_TIER2_LOOP_THRESHOLD` is misleadingly named. It sets a lower
`TierUpThreshold` for functions containing loops (`module.cpp:304`),
but the counter is the per-function **call counter in the prologue**
(`ir_builder.cpp:208-245`). A function with a hot loop that is called
once will never trip the threshold.

`emitLoopBackEdge()` (`ir_builder.cpp:2248`) collects SSA values for
PHI resolution and emits `ir_END()` -- no counter, no check.

**What's needed:** emit a counter increment + conditional check inside
`emitLoopBackEdge()`:

```
counter++
if (counter >= threshold && osr_entry_ptr != null):
    serialize locals to locals frame
    result = call osr_entry(exec_ctx, locals...)
    return result   // abandon tier-1 loop
// else: normal back-edge (fall through to LOOP_END)
```

**Effort: small.** ~50 LOC in `ir_builder.cpp`. The counter adds ~5
instructions per iteration (load, increment, store, compare,
branch-not-taken). On ctype's 50k-iteration loop this is ~0.1%
overhead -- negligible.

### Sub-problem 2: live variable capture

**This is the hardest sub-problem.** Wasm locals in tier-1 are pure SSA
values (`ir_builder.cpp:243: std::map<uint32_t, ir_ref> Locals`). They
are not in a frame structure or addressable memory. After register
allocation they're scattered across machine registers and spill slots,
with the mapping varying per function, per loop, and per back-edge.

Three approaches were evaluated:

**Approach A: serialize locals to memory at every back-edge (recommended).**

Modify `emitLoopBackEdge()` to emit `ir_STORE` for each live local to a
known buffer (a "locals frame" area in `JitExecEnv`, or reuse `args[]`).

- Pro: simple, clean contract. The OSR transition reads locals from
  a fixed location. No register allocator introspection needed.
- Con: extra stores per iteration. For a function with 20 live locals,
  20 stores/iter. On ctype (~5 live locals at back-edge) this is ~15
  cycles/iter = ~750k cycles on 50k iterations = ~0.1% of 8M-us WT.
- Con: SSA values that were pure register values now have memory uses,
  which may increase register pressure in dstogov/ir's allocator.

**Approach B: side table from register allocator output (rejected).**

After compilation, extract `ctx->vregs[]` and `ctx->live_intervals[]`
to build a mapping `{localIdx -> register|stack_offset}` per back-edge
PC. The OSR stub reads values from the right registers/stack slots.

- Pro: zero cost on the fast path.
- Con: `ir_ctx` is destroyed after compilation -- must serialize the
  mapping before the context is freed. No infrastructure for this.
- Con: reading the caller's register file from a C function is
  fragile (requires either `setjmp`-style capture or asm stubs that
  save all registers before the transition call).
- Con: mapping varies per back-edge point within the same function,
  requiring a per-PC table.

**Approach C: dstogov/ir's built-in OSR infrastructure (not ready).**

dstogov/ir has `IR_SNAPSHOT` (deopt snapshots), `osr_entry_loads`
(live variable lists at entry points), `IR_GUARD` (side-exits), and
`ir_add_osr_entry_loads()` + `ir_emit_osr_entry_loads()` in
`ir_ra.c:439-496`. This was built for PHP's JIT deoptimization.

- Pro: infrastructure exists and integrates with the register
  allocator. `ir_add_osr_entry_loads()` knows which values are live
  and where.
- Con: designed for JIT -> interpreter deopt (going *down*), not
  tier-1 -> tier-2 (going *up*). Would need adaptation.
- Con: WasmEdge emits zero `IR_SNAPSHOT` or `IR_GUARD` nodes today.
  The infrastructure is dead code in our context.
- Con: `IR_GUARD` has known bugs (`notes/bugs/bugs_frontend.md:150` --
  near-path overflow on >2GB jumps, far-path broken). Would need
  fixing first.

**Recommendation: Approach A.** The performance cost is small and
predictable. Approaches B and C are cleverer but carry far more
integration risk. Serializing at back-edges is what JavaScriptCore's
DFG JIT does -- proven approach.

### Sub-problem 3: LLVM OSR entry point compilation

We need to compile a version of the function that can be entered at a
loop header with explicit local values as arguments, not at the
function entry.

**Approach: continuation function via modified mini-module.**

1. Synthesize an "OSR mini-module" for the target function.

2. Rewrite the target function's wasm body:
   - Change signature to `(local0, local1, ..., localN) -> ret`
   - Replace the body prefix (everything before the target loop) with
     `local.set` instructions that initialize locals from the new
     parameters
   - Keep the loop body and everything after it intact

3. Compile through the normal `LLVM::Compiler::compile()` + thunk + O2
   pipeline. The result: a function that takes locals as params, enters
   the loop, runs to completion, returns the function's return value.

**Why this works:** wasm's structured control flow guarantees the
operand stack is empty at a loop header (modulo loop parameters from
the multi-value proposal, which most sightglass kernels don't use).
Locals are the only state crossing the loop boundary. A function that
takes locals as parameters and branches to the loop is a complete
representation of the execution state.

**Alternative approach: LLVM IR post-processing.** Instead of AST
surgery, compile the function normally and add an OSR entry basic block
to the LLVM IR that branches to the loop header with caller-supplied
PHI values. The LLVM frontend produces predictable block names ("loop",
"loop.end"). This avoids wasm-level surgery but requires understanding
the frontend's IR structure.

**Complications:**
- Multiple nested loops: MVP restricts to outermost hot loop.
- Globals/memory/tables: accessed through `exec_ctx` parameter, no
  change needed.
- Function calls from within the loop: go through the normal call
  mechanism (FuncTable or inlined batch members), same as today.

**Effort: medium-large.** ~200 LOC in `tier2_compiler.cpp`. The AST
surgery is novel code with no precedent in the codebase. The LLVM IR
post-processing alternative may be cleaner.

### Sub-problem 4: OSR transition

Once the LLVM OSR entry is ready, the back-edge check in tier-1 needs
to:

1. Load a flag/pointer from `JitExecEnv` indicating the OSR entry is
   available (set by the background worker after compilation)
2. Serialize locals to the locals frame (sub-problem 2)
3. Call the OSR entry: `result = osr_entry(exec_ctx, l0, l1, ...)`
4. Return `result` from the tier-1 function (abandon the tier-1 loop)

Step 4 is a side-exit: instead of taking the loop back-edge, we take
an exit path that calls the OSR entry and returns its value. In dstogov
IR terms, this is a conditional branch at the back-edge -- the normal
path takes `ir_LOOP_END`, the OSR path calls the compiled entry and
emits `ir_RETURN`.

**Effort: medium.** ~100 LOC in `ir_builder.cpp`. The IR emission is
structurally similar to the existing tier-up prologue (conditional
branch + call + return), just at the back-edge.

---

## Blocker assessment

| Component | Blocks? | Notes |
|---|---|---|
| Back-edge counter | No | Straightforward IR emission |
| Live variable serialization | No | Approach A (stores at back-edge) is simple |
| LLVM OSR entry compilation | **Partial** | AST surgery or IR post-processing; no precedent in codebase |
| OSR transition | No | Similar to existing tier-up prologue logic |
| `ir_ctx` lifetime | No (with A) | Only matters for Approach B (side table) |
| LLVM ISel hang | **Investigate** | Worker hangs in ISel during shutdown (`_exit(0)` workaround). If OSR compilation triggers the same bug, the entry never becomes ready. Needs testing. |
| Thread safety | No | OSR flag/pointer uses atomic store (same pattern as FuncTable swap) |
| Nested loops | No | MVP: outermost hot loop only |
| Multi-value loop params | **Partial** | Wasm loops can have params. Most sightglass kernels don't. Must handle for correctness. |

**No fundamental blockers.** The partial blockers are engineering
challenges, not architectural impossibilities.

---

## Effort estimate

| Task | Est. LOC | Files | Risk |
|---|---:|---|---|
| Back-edge counter + OSR check | ~50 | ir_builder.cpp | Low |
| Locals frame in JitExecEnv | ~20 | ir_jit_engine.h | Low |
| Locals serialization at back-edge | ~80 | ir_builder.cpp | Low |
| OSR flag/pointer in JitExecEnv | ~20 | ir_jit_engine.h, tier2_manager.cpp | Low |
| OSR transition (side-exit IR) | ~100 | ir_builder.cpp | Medium |
| OSR mini-module synthesis (AST surgery) | ~200 | tier2_compiler.cpp | **High** |
| OSR entry thunk | ~100 | tier2_compiler.cpp | Medium |
| Integration with tier2_manager | ~80 | tier2_manager.cpp/h | Low |
| Testing + debugging (33 kernels) | ~200 | test code | **High** |

**Total: ~850 LOC across 5 files.** The high-risk items are the AST
surgery and debugging correctness across diverse loop shapes. The
debugging/validation phase will likely take longer than the
implementation.

---

## Complementary change: caller-driven promotion

OSR fixes the *entry mechanism* (replacing the running loop body with
LLVM code). A separate improvement fixes *batch composition* (which
callees land in the same LLVM module): change the promotion trigger
from callee-driven to caller-driven. Instead of the leaf tripping
first and being promoted alone, the caller trips and pulls its hot
callees into the batch.

The same back-edge counter that triggers OSR can also trigger
caller-driven batch compilation. First trip: enqueue the caller + hot
callees for LLVM compilation. Second trip (after compilation): OSR
transition into the compiled loop body. See
`notes/issues/caller_driven_promotion.md` for the full design.

---

## What OSR does NOT help

- **Kernels where tier-1 already matches LLVM** (nestedloop, richards,
  keccak, etc.). OSR can't beat LLVM if tier-1 is already at parity.
- **Wide-call-graph kernels** (rust-html-rewriter). OSR helps the hot
  loop but can't fix compilation overhead.
- **Short-lived programs.** If the process exits before the OSR entry
  compiles, the transition never happens (same as the existing worker
  backlog problem).

---

## Implementation plan (proposed ordering)

1. **Add back-edge counter to `emitLoopBackEdge()`.** Verify the
   counter fires on ctype/ed25519/blind-sig loops. No OSR yet -- just
   prove the detection works.

2. **Add locals frame + serialization.** Emit stores at every
   back-edge. Verify locals are correctly serialized by reading them
   from a C callback at the back-edge check point. Measure tier-1
   overhead from the extra stores.

3. **Implement OSR mini-module synthesis.** Start with a single test
   case (ctype's `__original_main` loop). Compile the continuation
   function through the existing pipeline. Verify the LLVM IR looks
   correct (loop header with locals as params, callees inlined).

4. **Wire up the OSR transition.** Connect the back-edge check to the
   compiled entry. Run ctype end-to-end and compare WT against LLVM
   JIT. This is the first "does OSR actually work" milestone.

5. **Generalize and harden.** Test across all 33 sightglass kernels.
   Handle edge cases: nested loops, multi-exit loops, loops with
   multi-value params, loops calling imported functions.

6. **Measure and tune.** Full benchmark sweep. Measure OSR warmup
   cost, locals serialization overhead, and steady-state speedup.
   Tune the back-edge threshold.
