// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/peak_heap_tracker.h"

#include <stdio.h>

#include <memory>
#include <utility>

#include "absl/base/internal/spinlock.h"
#include "absl/memory/memory.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/stack_trace_table.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

bool PeakHeapTracker::IsNewPeak() {
  size_t current_peak_size = CurrentPeakSize();
  return (static_cast<double>(tc_globals.sampled_objects_size_.value()) >
          current_peak_size * Parameters::peak_sampling_heap_growth_fraction());
}

void PeakHeapTracker::MaybeSaveSample() {
  if (Parameters::peak_sampling_heap_growth_fraction() <= 0 || !IsNewPeak()) {
    return;
  }

  AllocationGuardSpinLockHolder h(&recorder_lock_);

  // double-check in case another allocation was sampled (or a sampled
  // allocation freed) while we were waiting for the lock
  if (!IsNewPeak()) {
    return;
  }
  SetCurrentPeakSize(tc_globals.sampled_objects_size_.value());

  // Guaranteed to have no live sample after this call since we are doing this
  // under `recorder_lock_`.
  peak_heap_recorder_.get_mutable().UnregisterAll();
  tc_globals.sampled_allocation_recorder().Iterate(
      [this](const SampledAllocation& sampled_allocation) {
        recorder_lock_.AssertHeld();
        StackTrace st = sampled_allocation.sampled_stack;
        peak_heap_recorder_.get_mutable().Register(std::move(st));
      });
}

std::unique_ptr<ProfileBase> PeakHeapTracker::DumpSample() {
  auto profile = std::make_unique<StackTraceTable>(ProfileType::kPeakHeap);

  AllocationGuardSpinLockHolder h(&recorder_lock_);
  peak_heap_recorder_.get_mutable().Iterate(
      [&profile](const SampledAllocation& peak_heap_record) {
        profile->AddTrace(1.0, peak_heap_record.sampled_stack);
      });
  return profile;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
