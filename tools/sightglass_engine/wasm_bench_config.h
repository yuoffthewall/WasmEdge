// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2024 Second State INC
//
// Mirror of `WasmBenchConfig` from sightglass
//   crates/recorder/src/bench_api.rs:9-37
//
// Layout MUST match sightglass exactly. Re-validate the byte size and field
// order whenever the pinned sightglass commit is bumped. The reference at the
// time of writing is a struct of:
//
//   ptr+len pairs for working_dir / stdout_path / stderr_path / stdin_path
//   three (timer, start_fn, end_fn) groups for compilation / instantiation /
//   execution phases
//   ptr+len pair for execution_flags
//
// On x86_64 Linux this is 16 + 16 + 16 + 16 + 24*3 + 16 = 152 bytes.

#pragma once

#include <cstddef>
#include <cstdint>

// Field names mirror sightglass's Rust struct verbatim so the ABI stays
// readable when cross-referenced. clang-tidy's identifier-naming check is
// suppressed for this struct only.
// NOLINTBEGIN(readability-identifier-naming)
extern "C" {  //NOLINT

struct WasmBenchConfig {
  const std::uint8_t *working_dir_ptr;
  std::size_t working_dir_len;

  const std::uint8_t *stdout_path_ptr;
  std::size_t stdout_path_len;

  const std::uint8_t *stderr_path_ptr;
  std::size_t stderr_path_len;

  const std::uint8_t *stdin_path_ptr;
  std::size_t stdin_path_len;

  std::uint8_t *compilation_timer;
  void (*compilation_start)(std::uint8_t *);
  void (*compilation_end)(std::uint8_t *);

  std::uint8_t *instantiation_timer;
  void (*instantiation_start)(std::uint8_t *);
  void (*instantiation_end)(std::uint8_t *);

  std::uint8_t *execution_timer;
  void (*execution_start)(std::uint8_t *);
  void (*execution_end)(std::uint8_t *);

  const std::uint8_t *execution_flags_ptr;
  std::size_t execution_flags_len;
};

static_assert(sizeof(WasmBenchConfig) == 152,
              "WasmBenchConfig layout drift — recheck against sightglass "
              "crates/recorder/src/bench_api.rs and bump the pinned commit");

} // extern "C"
