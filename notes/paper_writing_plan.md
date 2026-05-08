# Thesis Writing Plan (revised 2026-05-07)

**Title:** A Tiered Compilation Architecture for Low-Latency Wasm Runtimes in
Edge Computing Environments

**Author:** Hsiang-Yu Lee (李祥宇), R12922137
**Advisor:** Shih-Wei Liao (廖世偉)
**Institution:** National Taiwan University, Department of Computer Science
and Information Engineering
**Template:** NTU-Thesis (XeLaTeX + BibTeX), located at `~/Desktop/NTU-Thesis/`

---

## Style direction (revised)

This thesis is structured as an **academic paper, not technical
documentation**. The writing emphasizes the high-level design of the
tiered compilation architecture and the rationale behind each design
choice. It deliberately avoids exhaustive enumeration of implementation
details: per-instruction lowering tables, internal class layouts,
dstogov/ir pass names, prologue construction sequences, and
mechanically-specified code paths belong in the source tree and the
project's design docs, not in the thesis body.

## Writing principles

The body chapters explain the architecture's ideas at a high level,
supplemented by pseudocode and diagrams where they clarify a non-obvious
mechanism. Two rules follow:

- **Do not throw code in sentences.** Code-formatted identifiers (e.g.,
  backend opcode names, internal helper-function names, AST class names,
  environment variables, runtime constants) do not belong in prose. The
  canonical thesis terminology — Tier-1, Tier-2, OSR, `fwd_thunk`,
  `t1_thunk`, `entry_thunk`, mini-module, `JitExecEnv`, `FuncTable`,
  `OsrEntryTable` — is the only exception, and these terms are introduced
  in dedicated sentences rather than dropped mid-clause. Specific
  function-pointer offsets, struct field arrangements, and array index
  expressions belong in pseudocode or algorithmic blocks, not in prose.

- **Do not dive into technical details.** Backend pass names (`ir_match`,
  `ir_sccp`, `LLVMRunPasses`), internal helper functions (`jit_bounds_check`,
  `LLVMDeleteBasicBlock`), AST class names (`WasmToIRBuilder`,
  `Tier2Compiler`), exact byte offsets, environment-variable knobs, and
  specific measured numbers within the methodology section all belong in
  the source tree's design documents. The thesis says *what* the mechanism
  does and *why*; the source code and design docs say *how*. Specific
  measurements live in the Evaluation chapter, not in Methodology.

When a non-obvious mechanism requires concrete structure to be
understood, the chapter uses a pseudocode listing or an algorithmic
block. Pseudocode shows the *shape* of an emitted runtime sequence or the
*steps* of a compile-pipeline operation — not real production code, real
type names, or real backend opcodes. Pseudocode identifiers are abstract
(e.g., `notify_tier_up(funcId)` rather than `jit_tier_up_notify(env,
funcIdx, counterVal)`).

## Writing guidelines

- **Academic precision.** Formal, objective, precise language. Avoid
  hyperbolic terms ("amazing", "revolutionary"); prefer "efficient",
  "low-latency", "performant".
- **Active voice.** "The proposed architecture optimizes…" rather than "It
  is optimized by…".
- **High-level focus.** Describe mechanisms at the level of architectural
  patterns and design decisions, not at the level of individual functions
  or code paths.
- **Terminology consistency.** Strict adherence to the systems vocabulary
  fixed by the design docs:
  - **Tier-1** = the dstogov/ir baseline JIT.
  - **Tier-2** = the LLVM-frontend selective recompilation path (never
    "AOT" — AOT means whole-module).
  - **IR JIT** when distinguishing the tier-1 implementation from the
    dstogov/ir library itself.
  - **OSR** = on-stack replacement; **OSR continuation entry** = the
    compiled landing point; **back-edge** = the polling site;
    **transition** = the actual mid-loop migration.
  - **fwd_thunk** (tier-1 ABI → tier-2 LLVM ABI), **t1_thunk** (tier-2 →
    tier-1, in-place rewrite), **entry_thunk** (LLVM-ABI wrapper for
    `call_indirect` targets) — distinct, never interchangeable.
  - **FuncTable**, **OsrEntryTable**, **JitExecEnv** for the runtime
    structures.
  - **Mini-module** for the synthesized `AST::Module` fed to the LLVM
    frontend; **batch** for the set of functions promoted in one tier-up
    event.
  - **Cold start** for first-call latency; **steady-state** for post-warmup
    behavior; **WT** when citing sightglass wall-time.

---

## Chapter structure (revised: 6 chapters, ~57 pp body)

The structure shifts from an implementation-thesis layout (9 chapters,
three per-tier deep-dive chapters) to a paper-style layout. Three
contributions remain — tier-1 baseline JIT, tier-2 selective recompile,
and OSR — but each is presented as a section of the Methodology chapter
rather than as its own chapter.

### Ch 1 — Introduction (~6 pp)

Unchanged from previous plan. Already written.

- 1.1 Motivation. 1.2 Problem statement. 1.3 Contributions.
  1.4 Thesis organization.

### Ch 2 — Background and Related Work (~12 pp)

Merged from the previous Ch 2 (Background) and Ch 3 (Related Work).

- **2.1 The WebAssembly execution model.** Modules, function types, linear
  memory, tables, structured control flow.
- **2.2 The WasmEdge runtime.** Loader → validator → executor pipeline;
  the existing LLVM AOT/JIT compiler.
- **2.3 LLVM compilation cost.** Why a single-tier LLVM path is structurally
  unsuited to cold-start workloads.
- **2.4 dstogov/ir.** SSA graph IR and its optimization pipeline at a high
  level (no exhaustive pass enumeration); rationale for choosing it as
  the baseline-tier code generator.
- **2.5 Tiered compilation principles.** Profile-guided promotion, baseline
  vs. optimizing tiers, function-entry vs. on-stack-replacement.
- **2.6 Related work.** Tiered compilation in JS engines (V8, JSC,
  SpiderMonkey) and Wasm runtimes (Wasmtime, V8 Liftoff); OSR designs
  (HotSpot, V8); lightweight JIT libraries (dstogov/ir, Cranelift).

### Ch 3 — System Architecture (~10 pp)

The high-level design chapter. Sets up the contracts and the pipeline,
without descending into per-instruction lowering or code-path mechanics.
This chapter answers: *what are the components, how do they fit
together, and what interfaces do they expose to each other?*

Largely the existing `chapter04.tex` content, trimmed by ~20% to remove
implementation-leaning passages. Detailed plan in the next section.

### Ch 4 — Methodology (~16 pp)

The new consolidated chapter that replaces the previous Ch 5 (Tier-1),
Ch 6 (Tier-2), and Ch 7 (OSR). Three sections, one per contribution,
each focused on the *design choices that distinguish the proposed
mechanism* rather than the full implementation surface. Detailed plan
in the next section.

### Ch 5 — Evaluation (~10 pp)

Compressed from the previous Ch 8.

- **5.1 Methodology.** Sightglass benchmark suite, hardware setup, build
  configurations.
- **5.2 Tier-1 baseline.** Compile time and steady-state speed vs. LLVM
  AOT.
- **5.3 Tier-2 effectiveness.** Geomean tier-1/tier-2 and LLVM/tier-2
  ratios; per-kernel breakdown for representative kernels.
- **5.4 OSR effectiveness.** Loss-cluster case studies (ctype, ed25519,
  blind-sig).
- **5.5 Cold-start latency.** Instantiation + first-N-call latency.
- **5.6 Sensitivity and threats to validity.** Threshold and opt-level
  sensitivity, single-host-arch caveat.

### Ch 6 — Conclusion (~3 pp)

Unchanged in scope.

- 6.1 Summary. 6.2 Limitations. 6.3 Future work.

---

## Detailed plan for Chapter 3: System Architecture

The current `contents/chapter04.tex` already contains a good first draft
of System Architecture. The rewrite trims and re-targets it as a Ch 3
that does \emph{only} contracts and high-level structure, with no
implementation specifics that overlap with Methodology.

### Sections (revised)

**3.1 Pipeline overview** (~2 pp). Keep the five-stage pipeline diagram
(load+validate → tier-1 compile-all → first invocation → profile
saturation → tier-2 compile and atomic publish). Keep the colored
threading distinction. Trim: the per-stage prose can drop from five
paragraphs to three; merge the trap-stub and AOT-coexistence paragraphs
into a single "scope" note.

**3.2 Runtime context: `JitExecEnv`** (~2 pp). Keep the three-group field
table (lookup tables / trampoline pointers / promotion state). Trim:
remove the lifetime discussion of `IRJitEnvCache` shared-pointer
ownership — that detail belongs in Methodology if at all. Keep the
single-struct rationale (one indirection from the function-prologue env
pointer reaches all module state).

**3.3 Calling conventions** (~2 pp). Keep the two-ABI definition
(tier-1 uniform `(JitExecEnv*, uint64_t*)`, tier-2 LLVM SysV) and the
three-thunk taxonomy table. Trim: the asymmetry-rationale paragraph
between `fwd_thunk` and `t1_thunk` can shorten to one paragraph; the
\texttt{entry\_thunk} discussion can drop the \texttt{NotNullBB}-path
detail.

**3.4 Profile collection** (~1.5 pp). Keep the two-counter-class
description and the unsigned-LT gating rationale. Trim: collapse the
prose on saturation semantics into the same paragraph as the counter
description.

**3.5 Worker organization** (~1.5 pp). Keep the single-thread design
choice, the two-queue layout (regular + OSR-priority), the OSR-first
priority rationale. Trim: the dedup-set discussion compresses to one
sentence; the shared-pointer ownership discussion moves to Methodology
or drops entirely.

### What gets cut from current chapter04.tex

- The "Stage 5" detailed prose on the atomic publication mechanism (the
  release-store/acquire-load argument) — deferred to Ch 4 §4.2 or §4.3.
- The full `IRJitEnvCache` shared-pointer rationale.
- The three-paragraph thunk-asymmetry exposition; replaced by one
  paragraph.
- The mention of `compileBatch` / `compileOsrEntry` and shared helpers —
  those are Methodology concerns.

Target: 8 pp final length (down from current ~10 pp).

---

## Detailed plan for Chapter 4: Methodology

This is a new chapter that replaces Ch 5 / Ch 6 / Ch 7 from the previous
plan. Three mechanisms, one section each, ~5 pp per section. The chapter
focuses on \emph{design rationale} and \emph{key mechanisms}, not on the
full lowering surface.

### Source material to draw from

- `notes/design_docs/tier2_v2_doc.md` — particularly the "Why O0 at the
  frontend, O2 after post-processing" section and the dead-stub-prune
  motivation.
- `notes/design_docs/osr_doc.md` — particularly the three-state encoding
  table, the locals-frame parameter widening table, and the §12.1 batch
  composition history.
- `notes/design_docs/PHI_doc.md` and `notes/design_docs/call_doc.md` —
  for tier-1 §4.1.
- The existing `contents/chapter05.tex` (Tier-1 deep dive) provides
  raw material to compress, not to copy verbatim.

### Section 4.1: Tier-1 Baseline JIT (~5 pp)

Goal: explain the design choices that make tier-1 viable as a
baseline-tier compiler in WasmEdge, without the full instruction-lowering
manual.

- **4.1.1 The dstogov/ir backend at a glance.** Why an SSA-based JIT
  library was chosen over alternatives (Cranelift, GNU Lightning, hand-
  rolled emitter). Single-paragraph summary of what the backend does
  and what configuration the runtime uses (\texttt{O2} default,
  \texttt{IR\_FUNCTION | IR\_FASTCALL\_FUNC}, \texttt{IR\_OPT\_INLINE}
  enabled). No exhaustive pass list.
- **4.1.2 Wasm-to-IR translation strategy.** The high-level approach: a
  single-pass walker that maintains the wasm operand stack and a
  per-construct label stack, lowers operations to single SSA nodes
  where possible, and falls back to extern-C helpers for type-system
  mismatches (e.g., unsigned division). Brief mention of the key
  design choices (selective PHI emission for written-only locals,
  \texttt{ir\_SWITCH} for \texttt{br\_table}) without enumerating
  every visitor.
- **4.1.3 Profile instrumentation in tier-1 code.** The decision to
  emit profile counters \emph{into} the JIT-compiled body rather than
  through external profile collection. Cost characterization (a
  function prologue runs three additional IR ops post-saturation; a
  back-edge runs three ops post-saturation under the OSR diamond).
  Forward-references the OSR diamond to §4.3 and the contract to §3.4.

What this section deliberately omits compared to the current
`chapter05.tex`: the full list of dstogov/ir passes; the per-opcode
visitor breakdown; the exact prologue construction order; the
\texttt{collectLocalWritesInSpan} algorithm; the \texttt{setjmp}
termination buffer details; the per-flavor call dispatch breakdown; the
trap-handling code paths. These are mentioned only when a higher-level
design point depends on them.

### Section 4.2: Tier-2 Selective Recompilation (~6 pp)

Goal: explain how the architecture reuses the LLVM frontend for hot-
function recompilation and the small set of design choices that make
this reuse correct and efficient.

- **4.2.1 Reusing the LLVM frontend through mini-module synthesis.**
  The central insight: rather than re-implement Wasm-to-LLVM lowering
  for tier-2, the architecture synthesizes per-batch
  \texttt{AST::Module}s in which non-batch function bodies are
  replaced with default-typed-return stubs, then drives the existing
  WasmEdge LLVM frontend on the synthesized module. The mini-module
  preserves module-wide function indices so the frontend's call
  lowering resolves correctly. Rationale for default-typed returns
  vs. \texttt{unreachable; end}: the latter caused noreturn inference
  to fold batch bodies into traps.
- **4.2.2 The O0-then-O2 ordering.** The most important correctness
  invariant in the tier-2 pipeline. Compile the mini-module at \texttt{O0}
  through the LLVM frontend, post-process with thunks and stub
  pruning, then run an \texttt{O2} optimization pipeline. Explain why
  \texttt{O2} at the frontend produces wrong code: the optimizer
  inlines stub returns and DCEs cross-tier call sites before the
  thunks are emitted. The shootout-sieve / \texttt{memcpy} miscompile
  serves as the concrete example.
- **4.2.3 ABI bridging through three thunks.** Defer the taxonomy to
  Ch 3 §3.3; here, focus on \emph{when} each thunk is emitted in the
  pipeline and how the in-place rewrite of \texttt{t1\_thunk}
  preserves call-site bindings without RAUW.
- **4.2.4 Batch composition.** Walk-up root + bounded BFS over direct
  callees, with a static-frequency inclusion gate. Rationale for the
  static-frequency gate: dynamic-counter-only inclusion produces
  fragmented batches in workloads where one helper crosses the
  threshold before its siblings have warmed up (the ed25519
  bootstrap-window pattern).
- **4.2.5 Atomic publication.** A one-paragraph treatment of the
  \texttt{FuncTable[i] = fwd\_thunk\_ptr} release store and the
  acquire-side load semantics on x86-64.

What this section deliberately omits: the dead-stub-prune
implementation, the ORC LLJIT setup details, the
\texttt{wasmedge\_tier2\_get\_jit\_env} TLS mechanics, and the
shutdown-handling protocol. The dead-stub prune is mentioned only as a
performance optimization in the pipeline's post-processing step, not as
a section unto itself.

### Section 4.3: On-Stack Replacement (~5 pp)

Goal: explain why function-entry promotion alone is insufficient and
how the OSR mechanism closes the gap.

- **4.3.1 The one-shot-caller problem.** Why function-entry promotion
  delivers ~0% on workloads where the hot site is a single long-
  running loop inside a once-called function. Concrete example: the
  ctype / ed25519 / blind-sig kernels. The motivation
  for OSR.
- **4.3.2 Back-edge instrumentation: the three-state sentinel
  diamond.** Encoding rationale: the \texttt{OsrEntryTable[i]} slot
  holds either sentinel \texttt{1} (\emph{counting}, run the back-edge
  counter), sentinel \texttt{0} (\emph{waiting}, fall through), or a
  function pointer $\geq 2$ (\emph{ready}, snapshot locals and
  tail-call). Per-iteration cost in each state: three IR ops
  (\texttt{LOAD}, \texttt{TEST}, branch) on the post-saturation hot
  path. Cite the design-doc table on per-state cost.
- **4.3.3 OSR continuation entry synthesis.** The mini-module-style
  approach reused: lift wasm locals into LLVM function parameters,
  start the function body at the loop header, append the body as a
  new function slot rather than overwriting the original (so
  intra-function calls in the OSR body still resolve to the original
  function index). Structural bailouts for loops with non-empty block
  types or enclosing if/loop chains.
- **4.3.4 Locals serialization.** Type-native stores into the
  \texttt{OsrLocalsFrame}. Rationale: the dstogov/ir backend's
  foldable-op CSE deduplicates the widening operations across the
  transition path and the threshold-hit path, producing a single SSA
  definition reused in disjoint control branches and a downstream
  miscompile. Type-native stores avoid the CSE substrate entirely.
- **4.3.5 Worker-queue priority and batch composition.** OSR drains
  before regular tier-2; OSR uses the same BFS as regular tier-2 but
  with \texttt{SkipSeen=true} so already-promoted helpers are
  re-included in the OSR batch (enabling LLVM cross-function inlining
  inside the OSR body).

What this section deliberately omits: the validator-gate code path;
the exact field layout of the OSR fields in \texttt{JitExecEnv}; the
\texttt{LabelInfo::LoopIdx} bookkeeping; the OSR debug knobs
(\texttt{WASMEDGE\_OSR\_*} env vars).

---

## Implementation impact

The revision changes which `.tex` files exist and what each contains.

### Action items for Ch 3 / Ch 4 rewrite

1. **Move System Architecture content from `chapter04.tex` into
   `chapter03.tex`.** Currently `chapter03.tex` is placeholder; will
   become the new Ch 3 (System Architecture). Trim per the §3 plan
   above by ~20%.

2. **Rewrite `chapter04.tex` as Methodology.** Drops the System
   Architecture content (now in chapter03) and instead contains the
   three sections from the Ch 4 plan above. Drafted from a
   compression of the existing `chapter05.tex` (for §4.1), the
   `tier2_v2_doc.md` design doc (for §4.2), and the `osr_doc.md`
   design doc (for §4.3).

3. **Retire `chapter05.tex`** as a body chapter. Its content is too
   detailed for the new structure. Two options: (a) delete the file
   and remove the `\input{contents/chapter05}` line in `main.tex`;
   (b) repurpose `chapter05.tex` for the Evaluation chapter (Ch 5)
   and keep the file. Option (b) is cleaner because it keeps file
   numbering aligned with chapter numbering.

4. **`chapter02.tex` (Background and Related Work)** — currently
   placeholder; will be written later, merging the previous Ch 2 +
   Ch 3 content.

5. **Add `chapter06.tex` (Conclusion)** — currently does not exist;
   will be written last.

### Chapter / file mapping after revision

| Chapter | File | Status |
|---|---|---|
| Ch 1 — Introduction | `chapter01.tex` | Written (no change) |
| Ch 2 — Background and Related Work | `chapter02.tex` | Placeholder; rewrite later |
| Ch 3 — System Architecture | `chapter03.tex` | Move content from `chapter04.tex`, trim |
| Ch 4 — Methodology | `chapter04.tex` | Rewrite from scratch (compress current ch05 + design docs) |
| Ch 5 — Evaluation | `chapter05.tex` | Replace current Tier-1 content with Evaluation |
| Ch 6 — Conclusion | `chapter06.tex` | New file; write last |

`main.tex` updates to reflect the chapter06 input.

---

## Front and back matter

Unchanged from previous plan.

- **Abstract** (zh + en) — written.
- **Acknowledgement.**
- **Denotation.** Target ~25 entries: WASM, AOT, JIT, IR, SSA, ABI, OSR,
  CFG, PHI, SCCP, GCM, LSRA, ORC, LLJIT, plus thesis-specific terms
  (FuncTable, JitExecEnv, fwd_thunk, t1_thunk, entry_thunk, mini-module).
- **Appendices.** Two slots:
  - **A.** Full `JitExecEnv` C struct definition with field annotations.
  - **B.** Sightglass per-kernel raw numbers and extended tables that do
    not fit Ch 5.

---

## Open decisions

1. **Background + Related Work merged.** Resolved: merged (Ch 2).
2. **Discussion + Conclusion merged.** Resolved: merged (Ch 6).
3. **Three implementation chapters or one?** Resolved: one
   (Methodology / Ch 4).
4. **Architecture overview as its own chapter?** Resolved: yes (Ch 3).
5. **Page budget.** ~57 pp body.
6. **First-person plural vs. third-person.** Pending. Default in
   already-written chapters: third-person.

---

## Source material

- **Design docs.** `notes/design_docs/tier2_v2_doc.md`,
  `notes/design_docs/osr_doc.md`, `notes/design_docs/PHI_doc.md`,
  `notes/design_docs/call_doc.md`,
  `notes/design_docs/reg_based_call_doc.md`,
  `notes/design_docs/regular_tier2_vs_osr_difference.md`.
- **Benchmarks.** `notes/benchmarking/`.
- **Code references.** Tier-1 in `lib/vm/ir_builder.cpp`,
  `lib/vm/ir_jit_engine.cpp`. Tier-2 in `lib/vm/tier2_compiler.cpp`,
  `lib/vm/tier2_manager.cpp`. Executor wiring in
  `lib/executor/instantiate/module.cpp`, `lib/executor/helper.cpp`. The
  IR backend lives in `thirdparty/ir/`.

---

## Template housekeeping

Already applied:
- Removed duplicate `\input{contents/chapter03}` in `main.tex`.
- Replaced placeholder `keywords` and `keywords*` in `ntusetup.tex`.
- Added `\usetikzlibrary{positioning}` to `ntusetup.tex`.

Still pending:
- `ntusetup.tex` — `College of Electronical Engineering` →
  `College of Electrical Engineering`; `Department of Computer Science
  an Information Engineering` → `… Computer Science and Information
  Engineering`.
- Replace placeholder `DOI` (`10.5566/NTU2018XXXXX`) once known.
- Set `date` and `oral-date` once the defense schedule is fixed.
