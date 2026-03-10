
## Bug summary

**Symptom:** With the IR JIT, `call_indirect` can fail with **`jit_host_call` returning early** with `reason: "no_ref"`. The reported `funcIdx` and `tableSlot` are huge (e.g. `3958164877`, `1810681229`) while the table size is small (e.g. 17), so the table index passed into the helper is wrong.

**Scope:** Affects workloads that use `call_indirect` (e.g. quicksort, heapsort, seqhash, ratelimit). Noop-style benchmarks that don’t use it already pass.

---

## What we know so far

1. **Pop order is correct**  
   We pop the table index first, then the args (same as the interpreter). Changing to “args first, then table index” was tried and reverted; it didn’t fix the wrong value.

2. **Stack depth is correct**  
   Instrumentation showed `stackDepth == expectedDepth` (e.g. 4 == 4, 2 == 2). The bug is not in stack depth or restore logic.

3. **Wrong value, not wrong path**  
   Forcing the table index to constant `0` removed `no_ref` but caused `no_funcInst` (table[0] null). So the failure is due to the **value** used as the table index, not the dispatch path.

4. **Table index comes from a LOAD**  
   For every `call_indirect` build, the table-index ref is an **IR_LOAD** (refOp 86). So the index is loaded from memory (param/local slot), not from a constant or a simple PHI in the logs we saw.

5. **LOAD address is not a simple ADD(ArgsPtr, const)**  
   We added logging for the LOAD’s address (op1) and, when it’s an ADD, the constant offset. **Every log had `loadOffsetBytes == 0xFFFFFFFF...`** and addrOp was 74, 88, 101, 103, 107, 108 (not IR_ADD 25). So either the address isn’t a plain ADD, or the constant is in a different form (e.g. folded, or non-const op2). We never observed a clear “param 0 vs param 1” offset in the logs.

6. **Working hypothesis**  
   The **popped ref** (the SSA value we use as the table index) can, after IR folding/optimization or PHI, end up reading the **wrong slot** (e.g. first param instead of the slot that holds the table index). So we pass the first argument (or other wrong value) as the table index and get a garbage `funcIdx`/`tableSlot`.

---

## Fixes tried (not successful)

1. **Locals[numParams]**  
   Use the first local variable as the table index instead of the popped ref. **Reverted**: in quicksort the table index comes from **memory** (`i32.load offset=32` from first param), not from a local, so this would be wrong.

2. **Temp-slot for table index**  
   Store the popped table index in an `ir_ALLOCA` and reload it right before building the host call, to avoid the value being clobbered by CalleeArgs stores. **Reverted**: quicksort still segfaulted; the wrong value is likely from the **LOAD** (wrong address) or PHI, not from register clobbering.

3. **CurrFuncNumParams**  
   Added and set in `initialize()`/`reset()` for potential use; no behavior change.

## Next steps

- **Root cause**: The table index at runtime is wrong (garbage or wrong slot). Either (a) the LOAD that produces it uses the wrong base (e.g. confused with ArgsPtr/CalleeArgs), or (b) PHI/merge gives the wrong ref, or (c) codegen passes the wrong register.
- **Defensive**: In `jit_host_call`, bounds-check `tableSlot` against table size and return 0 without crashing when out of range (already returns 0; ensure `getRefAddr` doesn’t fault on OOB).
- **Debug**: Dump compiled IR for a function that does `call_indirect` (e.g. with `ir_save` or env var) and compare with interpreter behavior; or add logging of the table-index value and base address at the LOAD that feeds the call.