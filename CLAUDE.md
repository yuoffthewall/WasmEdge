# Testing rules

Apply to any test run in this repo.

0. **Invoke the `sightglass` skill before running any sightglass/IR-JIT build or test command.** Do this even if you think you know the invocation — the skill holds the canonical commands and current env-var defaults. If the skill is already loaded this session, you can skip re-invoking.

1. **No Interpreter mode for benchmarks.** Minutes per kernel, useless signal. Use `WASMEDGE_SIGHTGLASS_MODE=IR_JIT` (or `JIT`/`AOT` to compare backends). For a non-JIT oracle, use standalone `wasmtime`.

2. **Never pipe test output for pass/fail.** `cmd | grep; echo $?` reports `grep`'s exit, not the test's. Redirect: `cmd > /tmp/test.log 2>&1; echo $?`. Use `set -o pipefail` only if you need streaming.

3. **Only run sightglass kernels.** Ignore other gtest targets (unit/spec/WASI tests) unless the user explicitly asks. They distract from the sightglass signal.

4. **Grep the log after every run — exit 0 is not success.**
   ```shell
   grep -iE 'dumped|error|failed|mismatch|warning' /tmp/test.log
   ```
   Success = exit 0 **and** no matches. The harness can print "core dumped" or per-kernel mismatches while gtest still returns 0 (burned us on blake3-scalar, rust-compression). Report any matches before claiming success.

# Debugging rules

5. **Skip O1. Prioritize O2.** O2 is what ships. O0 is a "passes off" sanity check. O1 rarely reveals bugs O2 doesn't, and doubles the debug surface.

6. **Revert ineffective fixes.** If an attempted fix doesn't turn the test green, Remove it before trying the next one. Do not stack speculative changes. Every retained change must be justified by a red→green transition.

7. **No workaround fixes — find the root cause.** Specifically forbidden:
   - **No signal/error-handler fallbacks** to a lower tier. If every failure falls back, the higher tier contributes nothing.
   - **No pass-shuffling across opt levels.** Disabling a pass at O2 while leaving it at O1 (or flattening all levels) makes the opt-level knob meaningless.
   - **No per-kernel skips** or shape-based early returns.
   - **No frontend workarounds for backend bugs.** If a bug originates in the IR backend, fix it in the backend. Keep the IR JIT frontend's implementation correct and canonical — do not distort frontend IR emission to dodge a backend defect.

   Root-cause workflow:
   1. Shrink the repro (kernel, IR, or single wasm function).
   2. Inspect IR dumps: `WASMEDGE_IR_JIT_DUMP=1` → `/tmp/wasmedge_ir_NNN_{before,after}.ir`. Find the pass that violates the invariant.
   3. If IR looks correct but execution is wrong, drop to assembly: Debug build + GDB, break on `wasm_jit_NNN` (= (NNN+1)th JIT'd function, registered via `ir_gdb_register`), then `disas $pc-64,$pc+64` and `info registers`. Catches codegen/regalloc bugs the IR dump misses.
   4. Fix at the point the invariant breaks — the commit should explain *why* the old code was wrong.
   5. Add a regression test.

   A temporary workaround is only acceptable if explicitly labeled as such in the commit message, alongside a plan for the real fix.

# Reference

`notes/test_commad_cheetsheet.md` — full build/test/GDB/env-var reference.
