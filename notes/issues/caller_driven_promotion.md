# Caller-driven promotion

## Problem

Tier-2 promotion is currently **callee-driven**: when a leaf function's
`CallCounters[leafIdx]` reaches the threshold, `jit_tier_up_notify`
fires and the leaf is the trigger for batch assembly. The walk-up + BFS
mechanism then tries to find a warm parent to batch with, but this is
a heuristic afterthought -- the trigger is still the leaf.

This causes singleton-batch fragmentation. Small scalar primitives
(ed25519 field ops, ctype byte predicates) get hot first -- their call
counts exceed their callers' counts because callers fan out across
multiple callees. They trip the threshold before parents do and get
promoted alone. The root-anchored BFS batching + `WARM_DIVISOR` tuning
partially mitigates this, but the fundamental signal is backwards.

HotSpot and V8 both use **caller-driven** promotion: the caller's
profile data (per-call-site frequency, type feedback) triggers
compilation of the caller, and the compiler decides which callees to
inline based on that profile. The callee's own counter is not the
primary signal.

## Current architecture

```
ir_builder.cpp:208-245  (prologue counter)
  CallCounters[FuncIdx]++
  if (CallCounters[FuncIdx] == threshold):
    jit_tier_up_notify(env, FuncIdx, counter)

helper.cpp:534-558  (notification)
  jit_tier_up_notify():
    CallCounters[FuncIdx] = UINT32_MAX   // saturate
    Tier2Manager::enqueue(FuncIdx, Mod, FuncTable, CallCounters)

tier2_manager.cpp:312-363  (batch assembly)
  enqueue(FuncIdx):
    walkUpRootLocked()   // try to find warm parent
    bfsDownBatchLocked() // collect neighbors
    // FuncIdx is always in the batch (guaranteed)
```

The walk-up mechanism (`tier2_manager.cpp:189-242`) looks for a warm
parent by checking `CallCounters[parentIdx] >= threshold / WarmDivisor`.
At `WarmDivisor=256`, the warm floor is ~39 calls. This catches parents
with moderate call counts but misses parents that are called once
(e.g., `__original_main` in sightglass kernels).

## Proposed change: promote the caller, not the callee

Change the promotion signal so the **caller** triggers compilation when
it is hot enough, and pull in all its hot callees:

```
ir_builder.cpp  (prologue counter -- unchanged)
  CallCounters[FuncIdx]++
  if (CallCounters[FuncIdx] == threshold):
    jit_tier_up_notify(env, FuncIdx, counter)

tier2_manager.cpp  (batch assembly -- changed)
  enqueue(FuncIdx):
    // FuncIdx is the triggering function.
    // Instead of walking UP to find a parent, BFS DOWN from FuncIdx.
    // FuncIdx IS the root -- its callees are the batch members.
    batch = bfsDown(FuncIdx, depth=2, max=12)
    // Filter: only include callees with CallCounters[calleeIdx] > warmFloor
    // This avoids pulling in cold callees that happen to be statically
    // reachable but not actually called.
```

The walk-up logic is removed entirely. The triggering function is
always the root of the batch, and its callees are included based on
their own call counts (as a filter, not as a trigger).

### What this changes

| | Callee-driven (current) | Caller-driven (proposed) |
|---|---|---|
| Trigger signal | Leaf hits threshold first | Caller hits threshold |
| Batch root | Walk-up heuristic (may miss) | Always the triggering function |
| Singleton risk | High (leaf promoted alone) | Low (caller batches its callees) |
| Walk-up logic | Required (complex, tuning-sensitive) | Eliminated |
| Callee filter | None (all static callees included) | Call-count based (only hot callees) |

### Why this helps

The singleton-fragmentation problem disappears by construction. The
caller is always the batch root, and its hot callees are pulled in
based on their actual call counts. There's no race between parent and
child thresholds because the parent IS the trigger.

For ed25519 field ops: instead of `fe_mul` tripping first (called 10k
times) and being promoted alone while `scalarmult` (called 300 times)
is still warming up, `scalarmult` trips at threshold=300 and pulls
`fe_mul` + `fe_sq` + `fe_add` into its batch. LLVM inlines the field
ops into `scalarmult`'s fwd_thunk (via P1c's `alwaysinline`),
producing a single optimized entry point.

### Why this does NOT close the LLVM gap

Caller-driven promotion improves **batch composition** but does not
change the **dispatch mechanism**:

- The caller's compiled code still enters through `fN_fwd_thunk`
  (only reached on the *next* call to the caller)
- For one-shot callers (`__original_main`), the fwd_thunk is never
  invoked -- the function is already mid-loop on the call stack
- Cross-batch calls still go through FuncTable indirection

This is the same function-entry swap limitation that OSR addresses.
Caller-driven promotion and OSR are complementary:

- **Caller-driven promotion** fixes batch composition (which callees
  land in the same LLVM module)
- **OSR** fixes the entry mechanism (replacing the running loop body
  with LLVM code)

### Knob elimination

The current callee-driven architecture has 7 promotion/batching knobs:

| Knob | Default | Purpose |
|---|---:|---|
| `WASMEDGE_TIER2_ENABLE` | off | Master switch |
| `WASMEDGE_TIER2_THRESHOLD` | 10000 | Prologue call-count threshold for non-loop functions |
| `WASMEDGE_TIER2_LOOP_THRESHOLD` | 1000 | Prologue call-count threshold for functions with loops |
| `WASMEDGE_TIER2_WARM_DIVISOR` | 256 | Walk-up warm floor = `THRESHOLD / WARM_DIVISOR` |
| `WASMEDGE_TIER2_WALKUP_DEPTH` | 1 | Max hops up the call graph to find a warm root |
| `WASMEDGE_TIER2_BFS_DEPTH` | 2 | Max BFS depth down from root to collect batch members |
| `MaxBatchSize_` | 12 | Hard cap on functions per batch (compile-time constant) |

These exist because the promotion signal fires on the wrong function,
creating a chain of compensating mechanisms:

1. **Leaf trips first** (fundamental issue) ->
2. Need `WARM_DIVISOR` to define "warm enough parent" ->
3. Need `WALKUP_DEPTH` to control how far up to search ->
4. Need `BFS_DEPTH` to control how far down to collect from root ->
5. Need `MaxBatchSize` to cap the result ->
6. Need `THRESHOLD` / `LOOP_THRESHOLD` split because loop-containing
   functions are called less often but are hotter

The `WARM_DIVISOR` saga from the benchmark doc
(`tier2_v2_vs_llvm_jit_benchmark.md`) illustrates the fragility: the
shipped default of 2 was ~30x too strict for loss-cluster kernels.
Walk-up fired on 2/39 batches. Finding the right value (256) required
a manual sweep across 5 settings. A different workload with a
different fan-out ratio would need a different divisor.

**With caller-driven promotion, most knobs disappear:**

| Current knob | Needed? | Why |
|---|---|---|
| `ENABLE` | Yes | Still a master switch |
| `THRESHOLD` | Yes | Still need a trigger threshold |
| `LOOP_THRESHOLD` | **Replaced** | Back-edge counter targets one-shot callers directly |
| `WARM_DIVISOR` | **Eliminated** | No walk-up, no warm floor |
| `WALKUP_DEPTH` | **Eliminated** | No walk-up |
| `BFS_DEPTH` | Simpler | Could default to 1 (direct callees only) since the trigger is the right root |
| `MaxBatchSize` | Yes | Still need a cap |

Knob count: 7 -> 3-4. The remaining ones (`ENABLE`, `THRESHOLD`,
`MaxBatchSize`, maybe `BFS_DEPTH`) are straightforward and don't need
per-workload tuning. No knob requires a sweep to find the right value.

### Implementation

**Threshold tuning.** Callers are called less frequently than their
callees (by definition of fan-out). If the threshold is 10000,
`__original_main` (called once) will never trip. Two options:

1. **Per-function adaptive threshold.** Functions containing loops get
   a lower threshold (already implemented as `LOOP_THRESHOLD`). Could
   extend to "functions with many callees get a lower threshold".

2. **Back-edge counter as trigger.** Instead of the prologue call
   counter, use a loop back-edge counter. When a function's loop
   iterates enough times, promote the function + callees. This
   naturally targets one-shot callers with hot loops -- exactly the
   case that matters.

   This overlaps with the OSR back-edge counter (sub-problem 1 in
   `osr.md`). The same counter can serve both purposes: trigger
   caller-driven batch compilation on first trip, trigger OSR
   transition on second trip (after compilation completes).

**Changes required:**

| File | Change | LOC |
|---|---|---:|
| `tier2_manager.cpp` | Remove `walkUpRootLocked()`, change `enqueue()` to BFS down from triggering function, add callee call-count filter | ~60 |
| `tier2_manager.h` | Remove walk-up fields from `ModuleCG`, simplify `Request` | ~20 |
| `module.cpp` | Adjust threshold logic (lower for callers with many callees, or switch to back-edge trigger) | ~20 |

**Effort: small-medium.** ~100 LOC. The walk-up + BFS logic simplifies
to just BFS-down-from-trigger. The env var knobs (`WARM_DIVISOR`,
`WALKUP_DEPTH`) can be removed.

### Interaction with OSR

If both caller-driven promotion and OSR are implemented:

1. Back-edge counter trips -> enqueue caller-driven batch compile
   (caller + hot callees, compiled as OSR continuation function)
2. Background worker compiles the batch via LLVM
3. Back-edge counter trips again -> OSR entry is ready, transition

The batch includes the caller itself plus its hot callees. LLVM
inlines the callees into the continuation function. After OSR
transition, the loop body runs on fully-inlined LLVM code -- identical
to what LLVM JIT produces.

This is the end state that matches HotSpot/V8:
- Caller-driven = batch composition matches what the optimizer needs
- OSR = hot loop runs on optimized code even for one-shot callers
- Inlining = zero call overhead for hot callees within the loop
