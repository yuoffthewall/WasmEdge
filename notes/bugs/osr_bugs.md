# OSR IR — Known Bugs

Bugs surfaced by the tier-1 OSR (On-Stack Replacement) back-edge instrumentation
emitted in `WasmToIRBuilder::emitLoopBackEdge` (`lib/vm/ir_builder.cpp`). The OSR
IR is a counter-increment diamond:

```
...loop body...
cnt = LOAD(back_edge_counter)
cnt' = ADD(cnt, 1)
STORE(back_edge_counter, cnt')
IF (cnt' == threshold)
  TRUE  -> call jit_osr_notify(env, funcIdx, loopIdx) -> END
  FALSE -> END
MERGE_2(TRUE_END, FALSE_END)
END   <- back-edge to LOOP_BEGIN
```

Each tier-1 outermost-loop back-edge now gets a TRUE/FALSE control-flow diamond
inside the loop body. The frontend only emits this when
`WASMEDGE_OSR_THRESHOLD > 0`.

---

## Bug 1: Dead-PHI DCE corrupts codegen when OSR diamond is present

**Status: workaround reverted. Does not reproduce in the supported
configuration (tier2+OSR together). Retained as a documented hazard of
running OSR IR through `ir_iter_opt` without an accompanying tier-2
promotion path. Root cause not isolated.**

### Syndrome

The Sightglass `regex` kernel miscompiles at O2 under `IR_JIT` only when OSR IR
is emitted (`WASMEDGE_OSR_THRESHOLD > 0`). The golden expected output is:

```
[regex] matching default.input
[regex] found 92 emails
[regex] found 5301 URIs
[regex] found 5 IPs
```

Observed under OSR-enabled O2:

```
[regex] matching default.input
[regex] found 3 emails
[regex] found 1 URIs
[regex] found 2 IPs
```

Execution doesn't crash — a wasm-level `unreachable` is reached in an error
arm of the parser, producing a trap (`Code: 0x40a`) that the host logs and the
kernel re-enters with a degraded result.

Repro:

```shell
cd build
WASMEDGE_OSR_THRESHOLD=5000 \
WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
WASMEDGE_IR_JIT_OPT_LEVEL=2 \
WASMEDGE_SIGHTGLASS_KERNEL=regex \
./test/ir/wasmedgeIRBenchmarkTests --gtest_filter='*SightglassSuite*'
```

The bug does not reproduce:
- Without OSR (`WASMEDGE_OSR_THRESHOLD` unset or `0`) — same regex kernel at O2
  produces the correct output.
- At O0 or O1 with OSR — the SCCP iter-opt pass only runs for `opt_level > 1`.
- On the smaller `regex-redux` kernel, fn 252 alone can be pinned down with
  tighter gates (see §Investigation below); other kernels in the 33-kernel
  Sightglass suite appear clean.

### What the investigation confirmed

Bisected SCCP's `ir_iter_opt` (`thirdparty/ir/ir_sccp.c`) using env-var gates on
the DCE branch at line ~3670, gated further by a per-compile monotonic function
counter:

- `WASMEDGE_IR_SKIP_PHI_DCE=1` (skip DCE of dead PHIs in `ir_iter_opt`) → full
  fix for regex with OSR, zero regressions across the full 33-kernel suite.
- `WASMEDGE_IR_SKIP_DUP_PHI_DCE=1` (skip DCE only for PHIs whose data inputs
  contain duplicates, e.g. `PHI/3(merge, A, B, B)`) → fixes `regex-redux` but
  NOT `regex`. Proved there is at least one dead non-duplicate PHI in the regex
  kernel that also triggers the bug.
- `WASMEDGE_IR_SKIP_DCE=1` (skip all dead-code DCE, not just PHI) → fix.
- All other `ir_iter_opt` transformations (fold, block-merge, merge-opt,
  loop-opt, mem-opt, if-opt) are NOT the trigger — disabling them individually
  does not change the failure mode.

Inside `regex-redux`, the single offending compile is function-counter 252
(fn 252 under the OSR-enabled compile ordering). That function contains five
3-way merges (`MERGE/3`) and a 4-way merge (`MERGE/4`). The merges host 2, 15,
12, 2, 16 and 20 PHIs respectively. Many PHIs at `MERGE/3 l_1357` and
`MERGE/4 l_1394` have the duplicate-input pattern — two or more data positions
carrying the same value, e.g.:

```
l_1357 = MERGE/3(l_1068, l_1345, l_1356);
d_1358 = PHI/3(l_1357, d_1058, d_1340, d_1340);   // positions 2,3 dup
...
l_1394 = MERGE/4(l_288, l_1073, l_1141, l_1393);
d_1396 = PHI/4(l_1394, d_6, d_1058, d_6, d_1358); // positions 1,3 dup
d_1414 = PHI/4(l_1394, c_12, d_298, c_12, d_298); // two dup pairs
```

These PHIs become dead (use-count 0) after `ir_sccp_transform`, enter the DCE
worklist, and are removed by `ir_iter_remove_insn`. When that removal happens,
codegen downstream produces a jump that lands on a wasm `unreachable` block
at runtime.

IR structure diff between "DCE runs" and "DCE skipped" versions of fn 252
(`/tmp/f252_after_trap.ir` vs `/tmp/f252_after_skipped.ir`):

- Identical control-flow graph shape (same number of `MERGE`, `LOOP_BEGIN`,
  `IF`, `UNREACHABLE`, `END` nodes).
- Identical memory-op count (`CALL`/`STORE`/`LOAD`).
- The DCE'd version is shorter by exactly the dead PHIs + their cascaded dead
  inputs (e.g. `d_79 = SHL(d_10, c_17)` whose only user was a dead dup PHI).

So the IR *should* be semantically equivalent. The removed instructions are
genuinely dead (use-lists all zero). Yet the emitted machine code diverges in
a way that flips a branch at runtime.

### Where the dup-input PHIs come from

`WasmToIRBuilder::mergeLocals` (lib/vm/ir_builder.cpp:~2196) emits one PHI per
wasm local at every merge that has >1 predecessor:

```cpp
bool AllSame = true;
for (const auto &PathLocals : EndLocals) {
  ...Values.push_back(it->second);...
}
if (AllSame) { Merged[LocalIdx] = FirstVal; }
else         { Merged[LocalIdx] = emitPhi(LocalType, Coerced); }
```

`AllSame` only short-circuits when *every* predecessor produces the identical
value. If two of three predecessors share a value (e.g. `A, B, B`), the frontend
correctly emits `PHI/3(merge, A, B, B)` — valid SSA, non-minimal form. This
pattern exists in non-OSR builds of the same kernel, but with OSR the extra
control-flow diamonds shift which values flow through which merges; in
practice, many more of these PHIs become *dead* under the OSR build than under
the non-OSR build, exposing the DCE path.

### What the root cause is NOT

- Not a bug in `ir_use_list_remove_one` (ir.c:1347). Verified that it removes
  one occurrence per call, so repeated calls from `ir_iter_remove_insn` handle
  duplicate-input PHIs correctly (count decrements from 2 → 1 → 0).
- Not a bug in `ir_sccp_replace_insn`'s dup-use handling. `ir_insn_find_op`
  returns the first unreplaced occurrence, and the outer loop iterates
  use_list entries separately, so each occurrence gets updated and
  `ir_use_list_add` called the correct number of times.
- Not a bug in `ir_try_remove_empty_diamond`. The 3-way merges in fn 252 have
  predecessors from independent `IF` nodes, not from a single `SWITCH`, so the
  N-diamond path returns 0 (no change).
- Not the `ir_iter_optimize_merge` cascade following PHI DCE — it walks the
  same structurally-unchanged 3-way merge and returns without transforming.

### What it probably IS (untested hypotheses)

1. **Downstream pass consumes a pointer to a removed PHI**. `ir_iter_remove_insn`
   sets the insn's `opt = IR_NOP` but leaves it in `ir_base[]`. A later pass
   (GCM, schedule, match, live-range analysis, coalesce, regalloc, or
   schedule-blocks — see `thirdparty/ir/ir.h:1054-1062`) may dereference the
   node through a cached handle. The two recent dead-PHI fixes in this
   codebase (commits `42fe7fc fix(ir): Skip DESSA moves for dead PHIs` and
   `1afb041 fix(ir-jit): Fix O1 DESSA duplicate-copy ... dead-PHI inverted
   live range`) point to this exact class of problem at register allocation
   time — dead PHIs that survive into RA clobbering live registers. Our bug
   may be the O2 analogue: PHIs removed *before* RA, but some data structure
   (cfg_map, live_intervals, a use-edges array) retaining a stale handle.

2. **Cascade order in the bit-queue**. `ir_iter_remove_insn` queues the PHI's
   `op1` (the merge) plus any input that drops to zero uses. For dup-input
   PHIs the same data input gets visited twice on the removal side. If the
   intermediate state between the two `ir_use_list_remove_one` calls is
   observed by a worklist peek somewhere, a stale count-of-1 could mislead
   `ir_iter_optimize_merge`'s `count == 2` branch or similar.

3. **Scheduling invariant violated**. The OSR back-edge diamond emits a
   MERGE_2 just before the back-edge `END`. Removing dead PHIs at *other*
   merges in the same function shifts instruction scheduling such that GCM
   places a load across the OSR diamond's control edge. No direct evidence
   yet.

### Current status

**No workaround in the tree.** The previous `WASMEDGE_IR_SKIP_PHI_DCE`
gate (both the auto-setenv in `lib/executor/instantiate/module.cpp` and
the `getenv` branch in `thirdparty/ir/ir_sccp.c`) has been **reverted**.

In the supported OSR configuration — `WASMEDGE_TIER2_ENABLE=1` alongside
`WASMEDGE_OSR_THRESHOLD=N` — the bug does **not** reproduce. Tier-2's
function-entry swap promotes the hot regex functions to LLVM-compiled
code before `OSR_THRESHOLD` iterations accumulate, so the tier-1 IR
containing the OSR diamond is never on the hot path long enough for the
dead-PHI DCE to matter at runtime. The 30/33 sightglass-strong pass
rate under tier2+OSR confirms this empirically (residual failures are
tracked under Bug 2 and are unrelated to PHI DCE).

The failure remains reproducible **only** in OSR-without-tier2 runs,
which is not a supported configuration (OSR cannot compile continuation
entries without the tier-2 worker — see feedback note
`OSR testing requires tier2`). Such runs emit the OSR diamond into
tier-1 IR but can never transition out, so the miscompile surfaces with
no benefit to offset it.

### Plan for the real fix

Lower priority now that the bug is dormant under tier2+OSR. If it ever
surfaces again (e.g. a kernel where tier-2 doesn't race ahead of OSR):

1. Reproduce on a minimal hand-written IR with the OSR diamond + dup-PHI
   pattern outside the WasmEdge frontend. The upstream `dstogov/ir`
   `examples/` harness should drive `ir_sccp` + `ir_gcm` + `ir_emit` in
   isolation for a <200-line reproducer suitable for upstream.

2. Bisect the pipeline after SCCP with dead-PHIs left in place vs removed:
   dump after GCM, after schedule, after match, after RA; the first pass
   whose output structurally diverges beyond renumbering is the culprit.
   Instrumentation: `WASMEDGE_IR_JIT_DUMP=1` plus a per-pass snapshot.

3. If the divergence is register-allocation time, check whether the fixes
   in `42fe7fc` / `1afb041` need to also cover the "dead PHI was DCE'd to
   IR_NOP" state, not just "dead PHI survived to RA with count==0".

### Verification checklist

- [x] sightglass-strong at O2 with `TIER2_ENABLE=1 TIER2_THRESHOLD=10
      OSR_THRESHOLD=5000` — 30/33 green (residuals are Bug 2, not Bug 1).
- [x] Baseline tier-1 WT not regressed (counter is ~5 insns/iter on the
      OSR-instrumented back-edge; not reached on non-loop paths).
- [x] `WASMEDGE_IR_SKIP_PHI_DCE` workaround removed from both sides.
- [ ] Root cause identified (deferred; bug dormant in supported config).

### Related upstream commits (wasmedge)

- `42fe7fc fix(ir): Skip DESSA moves for dead PHIs to prevent register
  clobbering` — direct analogue at O1 for dead PHIs that survive DCE.
- `1afb041 fix(ir-jit): Fix O1 DESSA duplicate-copy assertion and dead-PHI
  inverted live range` — dead-PHI live-range handling in RA.
- `3ce1a78 fix(ir_load): Raise PHI/MERGE operand cap from 255 to 65535` —
  unrelated to this bug, but touches the PHI code path.

### Debug artifacts retained

In `/tmp/` (can be regenerated):
- `f252_before.ir` — fn 252 of `regex-redux` before `ir_iter_opt` DCE
- `f252_after_skipped.ir` — fn 252 with dup-PHI DCE skipped (correct codegen)
- `f252_after_trap.ir` — fn 252 with DCE run (runtime trap)
- `f252_phi_refs.txt` — 44 dead PHI refs in fn 252 (ref range 150..1388)
- `dce_fn252.log` — DCE trace per PHI kill

The `f252_before.ir` vs `f252_after_*.ir` diff is instructive: ~2700 lines of
renumbering, but structurally the only change is removed dead data
instructions. All `MERGE`, `LOOP_BEGIN`, `IF`, `END`, `STORE`, `CALL`, `LOAD`
counts are identical across the three dumps.

---

## Bug 2: OSR locals-store IR triggers O2 miscompile

**Status: majority fixed (31/33 kernels green). Two residual kernels
(blind-sig, rust-compression) still miscompile with a narrower, unresolved
signature. Main fix landed in `lib/vm/ir_builder.cpp` `emitLoopBackEdge()`;
`WASMEDGE_OSR_SKIP_STORES` bisection gate retained for the residual cases.**

### Syndrome (original, before fix)

With `WASMEDGE_OSR_THRESHOLD > 0` and the Phase-2 locals-serialisation
sequence emitting `ir_ZEXT_U64`/`ir_BITCAST_U64` widening + `ir_STORE` for
each local into `OsrLocalsFrame`, 13 of 33 sightglass kernels regressed
at O2 (SIGSEGV / trap / timeout / golden mismatch across blake3-scalar,
bz2, gcc-loops, pulldown-cmark, rust-html-rewriter, rust-protobuf,
blind-sig, rust-compression, rust-json, regex, shootout-ed25519,
shootout-keccak, shootout-random).

### Root cause (main 11/13)

dstogov/ir folds "foldable" ops — IR opcodes up to `IR_COPY`, including
`IR_ZEXT` and `IR_BITCAST` — through a CSE table (`_ir_fold_cse` in
`thirdparty/ir/ir.c`). When the OSR diamond emits stores on two sibling
control-flow branches (the transition TRUE path and the threshold-hit
TRUE path), each branch widens the *same* wasm local via
`ir_ZEXT_U64(local)` / `ir_BITCAST_U64(local)`. CSE sees the two widening
ops as equivalent (same opcode + same operand) and deduplicates them to a
single SSA def. That single def is then consumed by stores in two
*disjoint* control branches. Downstream, GCM chooses a placement for the
def (typically the common dominator above both branches), and later
passes (schedule / coalesce / regalloc) emit code where the widened
value's live range spans code paths that may not define it. The result
is wrong codegen on the non-threshold back-edge path — the path that
actually runs every iteration — even though the stores themselves are
semantically reachable only once per loop lifetime.

This matches the earlier observation that
`WASMEDGE_OSR_THRESHOLD=100000000` still reproduced the bug: it was
never about the stores *executing*, it was about the IR shape they
produced being shared across disjoint branches by CSE.

### The fix

`lib/vm/ir_builder.cpp` `emitLoopBackEdge()`: replace widening + 8-byte
store with a **type-native store** — `ir_STORE(slot, val)` at the
local's natural IR type (4 bytes for i32/f32, 8 bytes for i64/f64). The
upper 4 bytes of the slot are left stale for i32/f32, but the OSR thunk
(`tier2_compiler.cpp emitFwdThunk`) reads the full u64 slot and then
`LLVMBuildTrunc`s to the native parameter type, so the high bits are
ignored.

```cpp
for (const auto &[LocalIdx, Val] : Locals) {
  if (LocalIdx >= OSR_LOCALS_FRAME_SLOTS) break;
  ir_ref SlotAddr = ir_ADD_A(
      OsrLocalsFramePtr,
      ir_CONST_ADDR(static_cast<uintptr_t>(LocalIdx) * sizeof(uint64_t)));
  ir_STORE(SlotAddr, Val);   // native width; no ZEXT/BITCAST
}
```

Removing the widening ops removes the CSE substrate. `ir_STORE` is not
foldable — two stores to different addresses in disjoint branches cannot
be deduped — so the problematic single-def-feeding-two-branches shape
can no longer form.

Additionally, the redundant locals snapshot on the threshold-hit path
(originally Phase 2 behavior) was removed: Phase 4 re-stores locals
fresh on every iteration once the OSR entry is ready, so nothing
downstream reads from a pre-entry snapshot. Emitting those stores did
nothing useful and re-introduced the exact CSE shape above. The
threshold-hit path now only calls `jit_osr_notify`; the transition path
owns the snapshot.

### Verified

- 31/33 sightglass-strong kernels pass at O2 with
  `WASMEDGE_OSR_THRESHOLD=5000`. Exit-0 with no error/failed grep hits
  (modulo the "unreachable trap stubs" info line, which is harmless).
- Golden output unchanged across every passing kernel.
- No WT regression on non-looping functions (the widening ops were only
  on the OSR diamond; removing them cannot help or hurt non-OSR code).

### Residual failures under tier2+OSR (3/33)

Sweep config: `WASMEDGE_TIER2_ENABLE=1 WASMEDGE_TIER2_THRESHOLD=10
WASMEDGE_OSR_THRESHOLD=5000 WASMEDGE_IR_JIT_OPT_LEVEL=2`,
sightglass-strong, 60 s per-kernel timeout. 30/33 pass; 3 fail:

| Kernel             | Symptom                                  |
|--------------------|------------------------------------------|
| blind-sig          | timeout (hang under OSR; core dumped)    |
| shootout-base64    | timeout (hang under OSR; core dumped)    |
| shootout-ratelimit | timeout (hang under OSR; core dumped)    |

(The earlier OSR-only sweep also showed rust-compression trap at
FuncIdx 97/346; with tier2 enabled those functions get promoted before
OSR can fire into them, so the trap disappears — confirming the failure
class is OSR-specific and tier2 racing ahead masks it.)

Each of the three residual kernels fails at O2 in the supported
tier2+OSR config. All three pass with `WASMEDGE_OSR_SKIP_STORES=1`,
pointing the finger at the store chain emitted in the OSR diamond on
the hot tier-1 back-edge path (before tier-2 has landed the OSR entry,
and before tier-2 has function-entry-promoted the caller).

`rust-compression` bisected with `WASMEDGE_OSR_MIN_FUNC` /
`WASMEDGE_OSR_MAX_FUNC` down to **exactly two functions** out of 649:
`FuncIdx 97` and `FuncIdx 346`. Both have large store-chain IF_TRUE
branches (44+ stores) with loop-carried PHI values (`d_791`, `d_792`,
`d_793`) also used outside the diamond — the value lives across both
branches of the diamond *and* the enclosing loop.

The `WASMEDGE_OSR_SKIP_CALL` diagnostic (since removed) confirmed the
residual failures are driven by the **store chain in the IF_TRUE
branch**, not by the CALL+RETURN side-exit on the transition path.
Suppressing just the stores (`WASMEDGE_OSR_SKIP_STORES=1`) makes both
kernels pass; suppressing just the CALL does not.

Hypothesis: a secondary CSE/GCM interaction still exists for store
chains that use loop-carried PHI addresses or values, even without the
widening ops. Not yet isolated to a specific pass.

### Current state of the bisection gate

`lib/vm/ir_builder.cpp`:

```cpp
const char *SkipStoresEnv = std::getenv("WASMEDGE_OSR_SKIP_STORES");
const bool SkipStores = SkipStoresEnv && SkipStoresEnv[0] != '0';
if (!SkipStores) {
  /* emit type-native locals stores */
}
```

Retained as a bisection knob for debugging the residual 2 kernels. When
set, the transition path still runs but reads **stale** locals from
`OsrLocalsFrame`, so the OSR continuation produces wrong output —
debug-only, not a correctness gate.

No per-kernel skip, no pass disable, no kernel-shape early return in
the codebase. The residual failures are observable as natural outputs
from the 33-kernel sweep.

### Plan for the residual fix

1. Shrink `rust-compression` `FuncIdx=97` / `FuncIdx=346` to a
   standalone IR reproducer. The loop carries PHIs `d_791`, `d_792`,
   `d_793` through a 44-store IF_TRUE diamond branch — a <200-line
   hand-written IR harness should suffice.
2. Dump IR at each O2 pass boundary (`WASMEDGE_IR_JIT_DUMP=1` +
   per-pass snapshot patch). Identify the first pass whose output
   diverges between `SKIP_STORES=1` (correct) and `SKIP_STORES=0`
   (wrong).
3. Fix at the point the invariant breaks, in `thirdparty/ir/`.
4. Retire the `WASMEDGE_OSR_SKIP_STORES` gate and this residual
   subsection once green.

### Debug artifacts

Regenerated on demand via per-function OSR filters:

```shell
WASMEDGE_OSR_THRESHOLD=5000 \
WASMEDGE_OSR_MIN_FUNC=97 WASMEDGE_OSR_MAX_FUNC=97 \
WASMEDGE_IR_JIT_DUMP=1 \
WASMEDGE_SIGHTGLASS_KERNEL=rust-compression ...
```

produces `/tmp/wasmedge_ir_097_{before,after}.ir`.

### Related

- Bug 1 (dead-PHI DCE miscompile under OSR diamond) — same family:
  post-SCCP O2 pass misreasons about IR shape introduced by OSR. Bug 1
  is dormant in the supported tier2+OSR configuration; its workaround
  has been reverted.
