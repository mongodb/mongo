// Copyright 2023 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Instrumentation callbacks for SanitizerCoverage (sancov).
// https://clang.llvm.org/docs/SanitizerCoverage.html

#include "./centipede/runner_sancov_object.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "absl/base/nullability.h"
#include "./centipede/foreach_nonzero.h"
#include "./centipede/pc_info.h"
#include "./centipede/runner_dl_info.h"
#include "./centipede/runner_utils.h"

namespace fuzztest::internal {

void SanCovObjectArray::PCGuardInit(PCGuard *absl_nullable start,
                                    PCGuard *stop) {
  RunnerCheck((start != nullptr) == (stop != nullptr),
              "invalid PC guard table");
  skipping_no_code_dso_ = start == stop;
  if (skipping_no_code_dso_) return;
  // Ignore repeated calls with the same arguments.
  if (size_ != 0 && objects_[size_ - 1].pc_guard_start == start) return;
  RunnerCheck(size_ < kMaxSize, "too many sancov objects");
  auto &sancov_object = objects_[size_++];
  sancov_object.pc_guard_start = start;
  sancov_object.pc_guard_stop = stop;
  for (PCGuard *guard = start; guard != stop; ++guard) {
    guard->pc_index = num_instrumented_pcs_;
    ++num_instrumented_pcs_;
  }
}

void SanCovObjectArray::Inline8BitCountersInit(
    uint8_t *inline_8bit_counters_start, uint8_t *inline_8bit_counters_stop) {
  RunnerCheck((inline_8bit_counters_start != nullptr) ==
                  (inline_8bit_counters_stop != nullptr),
              "invalid 8-bit counter table");
  skipping_no_code_dso_ =
      inline_8bit_counters_start == inline_8bit_counters_stop;
  if (skipping_no_code_dso_) return;
  // Ignore repeated calls with the same arguments.
  if (size_ != 0 && objects_[size_ - 1].inline_8bit_counters_start ==
                        inline_8bit_counters_start) {
    return;
  }
  RunnerCheck(size_ < kMaxSize, "too many sancov objects");
  auto &sancov_object = objects_[size_++];
  sancov_object.inline_8bit_counters_start = inline_8bit_counters_start;
  sancov_object.inline_8bit_counters_stop = inline_8bit_counters_stop;
}

void SanCovObjectArray::PCInfoInit(const PCInfo *absl_nullable pcs_beg,
                                   const PCInfo *pcs_end) {
  RunnerCheck((pcs_beg != nullptr) == (pcs_end != nullptr), "invalid PC table");
  if (skipping_no_code_dso_) {
    RunnerCheck(pcs_beg == pcs_end,
                "unexpected non-empty PC table for no-code DSO");
    return;
  }
  const char *called_early =
      "__sanitizer_cov_pcs_init is called before either of "
      "__sanitizer_cov_trace_pc_guard_init or "
      "__sanitizer_cov_8bit_counters_init";
  RunnerCheck(size_ != 0, called_early);
  // Assumes either __sanitizer_cov_trace_pc_guard_init or
  // sanitizer_cov_8bit_counters_init was already called on this object.
  auto &sancov_object = objects_[size_ - 1];
  const size_t guard_size =
      sancov_object.pc_guard_stop - sancov_object.pc_guard_start;
  const size_t counter_size = sancov_object.inline_8bit_counters_stop -
                              sancov_object.inline_8bit_counters_start;
  RunnerCheck(guard_size != 0 || counter_size != 0, called_early);
  RunnerCheck(std::max(guard_size, counter_size) == pcs_end - pcs_beg,
              "__sanitizer_cov_pcs_init: mismatch between guard/counter size"
              " and pc table size");
  sancov_object.pcs_beg = pcs_beg;
  sancov_object.pcs_end = pcs_end;
  sancov_object.dl_info = GetDlInfo(pcs_beg->pc);
  RunnerCheck(sancov_object.dl_info.IsSet(), "failed to compute dl_info");
  if (sancov_object.pc_guard_start != nullptr) {
    // Set is_function_entry for all the guards.
    for (size_t i = 0, n = pcs_end - pcs_beg; i < n; ++i) {
      sancov_object.pc_guard_start[i].is_function_entry =
          pcs_beg[i].has_flag(PCInfo::kFuncEntry);
    }
  }
}

void SanCovObjectArray::CFSInit(const uintptr_t *cfs_beg,
                                const uintptr_t *cfs_end) {
  RunnerCheck((cfs_beg != nullptr) == (cfs_end != nullptr),
              "invalid control-flow table");
  if (skipping_no_code_dso_) {
    RunnerCheck(cfs_beg == cfs_end,
                "unexpected non-empty control-flow table for no-code DSO");
    return;
  }
  // Assumes __sanitizer_cov_pcs_init has been called.
  const char *called_early =
      "__sanitizer_cov_cfs_init is called before __sanitizer_cov_pcs_init";
  RunnerCheck(size_ != 0, called_early);
  auto &sancov_object = objects_[size_ - 1];
  RunnerCheck(sancov_object.pcs_beg != nullptr, called_early);
  sancov_object.cfs_beg = cfs_beg;
  sancov_object.cfs_end = cfs_end;
}

std::vector<PCInfo> SanCovObjectArray::CreatePCTable() const {
  // Populate the result.
  std::vector<PCInfo> result;
  for (size_t i = 0; i < size(); ++i) {
    const auto &object = objects_[i];
    for (const auto *ptr = object.pcs_beg; ptr != object.pcs_end; ++ptr) {
      auto pc_info = *ptr;
      // Convert into the link-time address
      pc_info.pc -= object.dl_info.link_offset;
      result.push_back(pc_info);
    }
  }
  return result;
}

std::vector<uintptr_t> SanCovObjectArray::CreateCfTable() const {
  // Compute the CF table.
  std::vector<uintptr_t> result;
  for (size_t i = 0; i < size(); ++i) {
    const auto &object = objects_[i];
    for (const auto *ptr = object.cfs_beg; ptr != object.cfs_end; ++ptr) {
      uintptr_t data = *ptr;
      // CF table is an array of PCs, except for delimiter (Null) and indirect
      // call indicator (-1). Convert into link-time address.
      if (data != 0 && data != -1ULL) data -= object.dl_info.link_offset;
      result.push_back(data);
    }
  }
  return result;
}

DsoTable SanCovObjectArray::CreateDsoTable() const {
  DsoTable result;
  result.reserve(size());
  for (size_t i = 0; i < size(); ++i) {
    const auto &object = objects_[i];
    size_t num_instrumented_pcs = object.pcs_end - object.pcs_beg;
    result.push_back({object.dl_info.path, num_instrumented_pcs});
  }
  return result;
}

void SanCovObjectArray::ClearInlineCounters() {
  for (size_t i = 0; i < size(); ++i) {
    const auto &object = objects_[i];
    if (object.inline_8bit_counters_start == nullptr) continue;
    const size_t num_counters =
        object.inline_8bit_counters_stop - object.inline_8bit_counters_start;
    memset(object.inline_8bit_counters_start, 0, num_counters);
  }
}

void SanCovObjectArray::ForEachNonZeroInlineCounter(
    const std::function<void(size_t idx, uint8_t counter_value)> &callback)
    const {
  size_t process_wide_idx = 0;
  for (size_t i = 0; i < size(); ++i) {
    const auto &object = objects_[i];
    if (object.inline_8bit_counters_start == nullptr) continue;
    const size_t num_counters =
        object.inline_8bit_counters_stop - object.inline_8bit_counters_start;
    ForEachNonZeroByte(object.inline_8bit_counters_start, num_counters,
                       [&](size_t idx, uint8_t counter_value) {
                         callback(idx + process_wide_idx, counter_value);
                       });
    process_wide_idx += num_counters;
  }
}

}  // namespace fuzztest::internal
