# Thesis Writing Plan (revised 2026-05-12)

**Title:** A Tiered Compilation Architecture for Low-Latency Wasm Runtimes in
Edge Computing Environments

**Author:** Hsiang-Yu Lee (李祥宇), R12922137
**Advisor:** Shih-Wei Liao (廖世偉)
**Institution:** National Taiwan University, Department of Computer Science
and Information Engineering
**Template:** NTU-Thesis (XeLaTeX + BibTeX), located at `~/Desktop/NTU-Thesis/`

---

## Style direction

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
mechanism. Three rules follow:

- **Be straight, understandable, and concise.** Use short sentences and
  plain words. Do not throw big words or use complex sentence structure.
  The reader should not have to re-read a sentence to parse it. When a
  paragraph is dense, break it into shorter paragraphs each making a
  single point.

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
  `ir_sccp`, `LLVMRunPasses`), internal helper functions
  (`jit_bounds_check`, `LLVMDeleteBasicBlock`), AST class names
  (`WasmToIRBuilder`, `Tier2Compiler`), exact byte offsets,
  environment-variable knobs, and specific measured numbers within the
  design chapter all belong in the source tree's design documents. The
  thesis says *what* the mechanism does and *why*; the source code and
  design docs say *how*. Specific measurements live in the Evaluation
  chapter, not in the design chapter.

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
- **No vague pronouns.** Avoid "the two X", "these Y", "the former and the
  latter". Repeat the antecedent noun even at the cost of a few extra
  words.
- **Terminology consistency.** Strict adherence to the systems vocabulary
  fixed by the design docs:
  - **Tier-1** = the dstogov/ir baseline JIT.
  - **Tier-2** = the LLVM-frontend selective recompilation path (never
    "AOT" — AOT means WasmEdge's existing whole-module compile-to-`.so`
    feature, separate from tier-2).
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

## Chapter structure (5 chapters, paper-style layout)

Three contributions (tier-1 baseline JIT, tier-2 selective recompile,
OSR) are folded into the single Design chapter as separate sections.
PDF length to date: 60 pages including front matter, body, references
stub, and Appendices A/B.

### Ch 1 — Introduction [WRITTEN]

File: `chapter01.tex`. Four sections, ~6 pp.

- §1.1 Motivation
- §1.2 Problem Statement — three sub-problems: baseline tier, optimizing
  tier, in-flight migration
- §1.3 Contributions — four enumerated
- §1.4 Thesis Organization

### Ch 2 — Background and Related Work [PLACEHOLDER]

File: `chapter02.tex`. Currently holds template placeholder text. ~12 pp
target.

Planned sections:

- §2.1 The WebAssembly execution model
- §2.2 The WasmEdge runtime
- §2.3 LLVM compilation cost
- §2.4 dstogov/ir at a high level
- §2.5 Tiered compilation principles
- §2.6 Related work (V8 / JSC / SpiderMonkey, Wasmtime, HotSpot OSR,
  lightweight JIT libraries)

### Ch 3 — Design [WRITTEN]

File: `chapter03.tex`. Five sections, 17 numbered subsections, ~18 pp.
Contains the architecture diagram (Figure 3.1), two structural tables,
two algorithms, and one pseudocode listing.

- §3.1 Architecture Overview — Figure 3.1 (tier-1, tier-2, OSR pipeline)
- §3.2 Runtime Context and Calling Conventions
  - §3.2.1 The JitExecEnv runtime context — Table 3.1 (field groups)
  - §3.2.2 Tier-1 calling convention
  - §3.2.3 Tier-2 calling convention
  - §3.2.4 Three thunk flavors — Table 3.2 (thunk taxonomy)
- §3.3 Tier-1 Baseline JIT
  - §3.3.1 Lightweight SSA backend as the code generator
  - §3.3.2 Single-pass wasm-to-IR translation
  - §3.3.3 Profile instrumentation embedded in compiled bodies
- §3.4 Tier-2 Selective Recompilation
  - §3.4.1 The tier-2 worker thread
  - §3.4.2 Mini-module synthesis — Algorithm 3.1
  - §3.4.3 Compile-ordering invariant
  - §3.4.4 ABI bridging in the post-processing pass
  - §3.4.5 Batch composition — Algorithm 3.2
  - §3.4.6 Atomic publication
- §3.5 On-Stack Replacement
  - §3.5.1 The one-shot-caller problem
  - §3.5.2 Three-state back-edge instrumentation — Listing 3.1
  - §3.5.3 Continuation entry synthesis
  - §3.5.4 OSR batch composition

### Ch 4 — Evaluation [STUB]

File: `chapter04.tex`. Chapter heading + placeholder paragraph. ~10 pp
target.

Planned sections:

- §4.1 Methodology — sightglass suite, hardware setup, build configurations
- §4.2 Tier-1 baseline — compile time and steady-state vs. LLVM AOT
- §4.3 Tier-2 effectiveness — geomean tier-1/tier-2 and LLVM/tier-2 ratios
- §4.4 OSR effectiveness — loss-cluster case studies (ctype, ed25519,
  blind-sig)
- §4.5 Cold-start latency — instantiation + first-N-call latency
- §4.6 Sensitivity and threats to validity — threshold and opt-level sweeps

### Ch 5 — Conclusion [STUB]

File: `chapter05.tex`. Chapter heading + placeholder paragraph. ~3 pp
target.

Planned sections:

- §5.1 Summary
- §5.2 Limitations — scalar-only promotion filter, no de-tiering,
  x86-64-only host support
- §5.3 Future work — v128/reftype scope, aarch64 port, refcounted JIT
  cache for de-tiering, profile-guided batch policy

### File-to-chapter mapping

| File | Chapter | Status |
|---|---|---|
| `chapter01.tex` | Ch 1 Introduction | Written |
| `chapter02.tex` | Ch 2 Background and Related Work | Placeholder |
| `chapter03.tex` | Ch 3 Design | Written |
| `chapter04.tex` | Ch 4 Evaluation | Stub |
| `chapter05.tex` | Ch 5 Conclusion | Stub |

---

## Front and back matter

- **Abstract** (zh + en) — written.
- **Acknowledgement** — template placeholder retained; to be drafted last.
- **Denotation** — template placeholder. Target ~25 entries: WASM, AOT,
  JIT, IR, SSA, ABI, OSR, CFG, PHI, SCCP, GCM, LSRA, ORC, LLJIT, plus
  thesis-specific terms (FuncTable, JitExecEnv, fwd_thunk, t1_thunk,
  entry_thunk, mini-module).
- **Appendices.** Two slots, template stubs:
  - **A.** Full `JitExecEnv` C struct definition with field annotations.
  - **B.** Sightglass per-kernel raw numbers and extended tables that do
    not fit Ch 4.

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
  `lib/executor/instantiate/module.cpp`, `lib/executor/helper.cpp`.
  The IR backend lives in `thirdparty/ir/`.

---

## Template housekeeping

Applied:

- `main.tex` — removed duplicate `\input{contents/chapter03}` line.
- `ntusetup.tex` — replaced placeholder `keywords` / `keywords*` with
  thesis keywords (WebAssembly, JIT, tiered compilation, OSR, edge
  computing, WasmEdge).
- `ntusetup.tex` — loaded `\usetikzlibrary{positioning}` for the
  architecture diagram.
- `ntusetup.tex` — loaded `algorithm`, `algpseudocode`, and `listings`
  packages for pseudocode and algorithm blocks.
- `ntusetup.tex` — `tocdepth = 2`, `secnumdepth = 2` so the TOC shows
  numbered subsections and stops there.
- `ntusetup.tex` — redefined `\paragraph` to invoke its starred form
  unconditionally, suppressing TOC pollution from `\paragraph` headings
  that titletoc/tocloft would otherwise force in regardless of tocdepth.
- `ntusetup.tex` — overrode `\appendix{NUM}{TITLE}` macro to refstep the
  chapter counter, so each appendix has a unique hyperref anchor
  namespace (the class macro originally renamed `\thechapter` without
  incrementing the underlying counter, causing A.1 and B.1 to collide
  on the same `section.5.1` anchor).

Pending:

- `ntusetup.tex` — `College of Electronical Engineering` →
  `College of Electrical Engineering`; `Department of Computer Science
  an Information Engineering` → `… Computer Science and Information
  Engineering`.
- Replace placeholder `DOI` (`10.5566/NTU2018XXXXX`) once known.
- Set `date` and `oral-date` once the defense schedule is fixed.
