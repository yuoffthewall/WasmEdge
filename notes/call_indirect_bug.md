Bug Summary: call_indirect Uses Wrong Index Space for FuncTable Lookup
Root Cause
In ir_builder.cpp:2765-2767, the call_indirect codegen uses the wasm table element index (popped from the wasm operand stack) to directly index into FuncTable, which is indexed by wasm function index. These are two different index spaces.


// BUG: TableIndex is a table element index, NOT a function index
Off = ir_MUL_A(ir_ZEXT_A(TableIndex), ir_CONST_ADDR(sizeof(void *)));
FuncPtr = ir_LOAD_A(ir_ADD_A(ValidFT, Off));  // FuncTable[tableElemIdx] -- WRONG
How It Manifests
In the richards benchmark, task handler function pointers (e.g. idlefn, workfn, handlerfn, devfn) are stored in wasm linear memory as table element indices (assigned by the wasm linker). When call_indirect executes:

The code loads table element index (e.g., 3) from memory (tcb->t_fn)
It looks up FuncTable[3], which is the native code for wasm function 3 (some unrelated function)
It calls the wrong function, which returns a garbage value (e.g., 0xf7ffa000 — the lower 32 bits of a JIT code pointer)
That garbage is used as a wasm memory offset, causing a segfault
The correct behavior would be: table element 3 → resolve through wasm table → function index N → FuncTable[N].

What Wasm call_indirect Actually Requires
Pop table element index from stack
Look up wasmTable[elementIdx] → get funcref (which encodes the wasm function index)
Type-check the funcref against the expected signature
Call the resolved function
Step 2 is missing in the JIT path. Direct call instructions are unaffected because they use compile-time-known function indices.

Impact
Any wasm module using call_indirect (function pointers, virtual dispatch, switch tables compiled to indirect calls) will call the wrong function, producing corruption or segfaults. This affects richards, regex, rust-json, and rust-protobuf benchmarks (all use indirect calls). Simple benchmarks without function pointers (e.g., hashset, shootout-*) are unaffected.

Verification
Forcing call_indirect to pass funcPtr=null (which falls back to jit_host_call, correctly resolving through the wasm table) fixed richards and rust-protobuf. regex and rust-json still crash — likely a second, unrelated bug in JIT codegen.

Proper Fix Direction
The fix needs to resolve the table element index to a function index at runtime before looking up FuncTable. Options:

Add an indirect call table to JitExecEnv: Pre-build a void* IndirectTable[] mapping table element indices → native code pointers. Use this for call_indirect instead of FuncTable. Needs invalidation if table.set/table.grow mutate the table.

Resolve at call site: Add the wasm table pointer to JitExecEnv. Generate IR to load wasmTable[elemIdx] → extract funcIdx → FuncTable[funcIdx]. More complex IR but avoids a separate table.

Always use host path for call_indirect (current workaround): Pass null funcPtr to force jit_host_call resolution. Correct but slower — richards times out at 35s with this approach.