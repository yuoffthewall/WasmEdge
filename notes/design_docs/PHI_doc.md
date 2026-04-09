---
name: PHI Node Bug Audit
overview: Audit of all PHI node usage in ir_builder.cpp, identifying 7 confirmed bugs and several improvements ranging from correctness-critical to robustness.
todos:
  - id: bug1
    content: Fix Block 2-path merge to iterate locals from both paths (use AllLocalIndices pattern)
    status: completed
  - id: bug2
    content: Fix Loop PHI to handle multiple back-edges (pre-merge back-edges into single MERGE node)
    status: completed
  - id: bug3-4
    content: Fix Block 2-path merge to use fresh MergedLocals map + handle partial-path locals in N-way merge
    status: completed
  - id: bug5-6
    content: Parse block type for loops and multi-value blocks
    status: completed
  - id: refactor
    content: Factor out duplicated PHI merge logic into helper function
    status: completed
isProject: false
---

# PHI Node Implementation Audit

All PHI-related logic lives in [lib/vm/ir_builder.cpp](lib/vm/ir_builder.cpp) with data structures defined in [include/vm/ir_builder.h](include/vm/ir_builder.h).

## Bug 1 (Critical): Block 2-path merge only iterates locals from path 0

**Location**: Lines 1833-1858 (`visitEnd`, `ControlKind::Block`, `EndList.size() == 2`)

```1833:1858:lib/vm/ir_builder.cpp
          for (auto &[LocalIdx, Val1] : Label.EndLocals[0]) {
            auto it2 = Label.EndLocals[1].find(LocalIdx);
            if (it2 != Label.EndLocals[1].end()) {
              // ...create PHI...
            }
          }
```

The loop only iterates locals present in `EndLocals[0]`. If path 1 has a local that path 0 does not (e.g., a `local.set` only happened inside a `br`-taken path), that local is silently dropped. Compare with the **If 2-path merge** (lines 2019-2057) which correctly collects `AllLocalIndices` from both paths. The Block case should do the same.

**Fix**: Replace the single-iteration loop with the same `AllLocalIndices` pattern used in the If case.

---

## Bug 2 (Critical): Loop PHI back-edge overwrite with multiple back-edges

**Location**: Lines 2151-2166 (`visitBr` to loop), 2203-2217 (`visitBrIf` to loop), 2275-2289 (`visitBrTable` to loop)

Every `br`/`br_if`/`br_table` targeting a loop calls:

```
ir_PHI_SET_OP(PhiRef, 2, CoercedVal);
ir_MERGE_SET_OP(Target.LoopHeader, 2, LoopEnd);
```

This always sets position **2** (the second operand). When a loop has **multiple back-edges** (e.g., `br_if 0` at the top and `br 0` at the bottom, or two `br_if 0` in different branches), each call **overwrites** the previous one. Only the **last** back-edge's values survive. The `LOOP_BEGIN` was created with `inputs_count=2` (entry + one back-edge), so there is only one slot for a back-edge at all.

This is a fundamental limitation: the IR library's `LOOP_BEGIN` is allocated with exactly 2 inputs. Supporting multiple back-edges would require either:

- Pre-merging all back-edges into a single `MERGE` node before the `LOOP_END`, or
- Re-allocating the `LOOP_BEGIN`/PHI nodes with more inputs.

For WebAssembly, a loop with a single `br_if` at the end is common and works fine. But any loop with multiple back-edges (e.g., `continue` from two different `if` branches) will silently produce wrong values.

**Fix**: Before creating a `LOOP_END` + `MERGE_SET_OP`, check if a back-edge has already been set on this loop. If so, merge the two back-edges into a `MERGE_2` node first, then set that as the single back-edge. Alternatively, the cleanest approach: insert a dedicated "back-edge merge" `MERGE` node at the loop tail, and have all back-edges merge into it before looping.

---

## Bug 3 (Medium): Block N-path merge drops locals not present in all paths

**Location**: Lines 1875-1912 (`visitEnd`, `ControlKind::Block`, `EndList.size() > 2`)

```1890:1895:lib/vm/ir_builder.cpp
          if (Values.size() == NumPaths) {
            if (AllSame) {
              Locals[LocalIdx] = FirstVal;
            } else {
              // ... create PHI_N ...
            }
          }
```

When `Values.size() != NumPaths` (a local exists on some paths but not all), the local is silently dropped from `Locals`. This can happen if a path terminates early via `return`/`unreachable` before setting a local. A safer approach: if a local is missing from one path, use its pre-block value (or the value from the first path that has it) rather than dropping it entirely.

The same issue exists in the If N-path merge (lines 2080-2117).

---

## Bug 4 (Medium): Block 2-path merge uses stale `Locals` as base

**Location**: Lines 1830-1863 (`visitEnd`, `ControlKind::Block`, `EndList.size() == 2`)

After `ir_MERGE_2`, the code iterates `EndLocals[0]` and creates PHIs, writing results directly into `Locals`. But `Locals` at this point still holds whatever state it had from the last-executed path (which may be path 0 or path 1 depending on control flow). Any local that is the same on both paths (`Val1 == Val2`) is set to `Val1`, but locals from path 1 that are NOT in path 0 are never written because the loop only iterates path 0's keys.

Combined with Bug 1, this means `Locals` after the merge is a mix of stale values and partial PHIs. It should be reset to a clean merged map, as the If 2-path case does with `MergedLocals`.

**Fix**: Use a fresh `std::map<uint32_t, ir_ref> MergedLocals` (like the If case on line 2015), collect all indices from both paths, and assign `Locals = MergedLocals` at the end.

---

## Bug 5 (Low): Loop Arity is hardcoded to 0

**Location**: Line 1662 (`visitLoop`)

```1662:1662:lib/vm/ir_builder.cpp
  Label.Arity = 0;  // TODO: Get from block type
```

WebAssembly loops can have result types (since the multi-value proposal). If a loop has `Arity > 0`, the result value at the `End` would need to be pushed. Currently, any loop with a result type silently drops its value. This could cause stack misalignment for downstream instructions.

**Fix**: Parse the block type just like `visitBlock` and `visitIf` do.

---

## Bug 6 (Low): `visitBlock` does not parse multi-value block types

**Location**: Lines 1628-1636 (`visitBlock`)

```1628:1636:lib/vm/ir_builder.cpp
  const BlockType &BType = Instr.getBlockType();
  if (!BType.isEmpty() && BType.isValType()) {
    Label.Arity = 1;
    Label.ResultType = wasmTypeToIRType(BType.getValType());
  } else {
    Label.Arity = 0;
    Label.ResultType = IR_VOID;
  }
```

Only `isValType()` (single-value) is handled. If the block has a type index (multi-value function type), `Arity` is set to 0. Multi-value blocks would silently drop their result values. The same applies to `visitIf` (lines 1727-1735).

**Fix**: Handle `BType.isTypeIndex()` by looking up the type's return types. For a first pass, at least support the single-return-value case from a type index.

---

## Bug 7 (Low): `visitEnd` for If without else pushes a default result but no PHI

**Location**: Lines 1976-1993 (`visitEnd`, If case, no else clause)

When an `if` has `Arity > 0` but no `else` clause, a default constant (zero) is pushed as the false-branch result, and both branch ends are collected. The merge logic then creates a PHI for the result. However, the default constant is always pushed into `BranchResults` even when the true branch terminated (e.g., via `return`). If `TrueBranchTerminated` is true and there's no else, then `EndList` has only 1 entry (the false branch), but `BranchResults` has 1 entry (the default). The merge logic checks `BranchResults.size() == 2` and fails, so the result is never pushed. This is subtly correct (single-path continuation uses `ir_BEGIN`, no PHI needed) but fragile -- the default value in `BranchResults` is wasted and could cause confusion if the logic changes.

---

## Improvement 1: Factor out common PHI merge logic

The N-way PHI merge code is duplicated between the Block case (lines 1868-1912) and the If case (lines 2072-2117). The 2-way PHI merge for the If case (lines 2014-2058) is also structurally identical to what the Block case should be doing. All of these should be factored into a helper like `mergeLocalsWithPHI(EndLocals, LocalTypes)`.

---

## Improvement 2: Use `ir_SWITCH` instead of chained if-else for `br_table`

`visitBrTable` (lines 2231-2330) uses chained `if-else` comparisons. The IR library provides `ir_SWITCH`/`ir_CASE_VAL`/`ir_CASE_DEFAULT` which would generate better native code (jump tables). This isn't a correctness issue but affects performance for large branch tables.

---

## Summary of priorities


| #   | Severity | Description                                       |
| --- | -------- | ------------------------------------------------- |
| 1   | Critical | Block 2-path merge only iterates path 0 locals    |
| 2   | Critical | Loop PHI overwrites with multiple back-edges      |
| 3   | Medium   | N-path merge drops locals missing from some paths |
| 4   | Medium   | Block 2-path merge uses stale Locals as base      |
| 5   | Low      | Loop Arity hardcoded to 0                         |
| 6   | Low      | Multi-value block types not parsed                |
| 7   | Low      | If-without-else default result handling fragile   |


