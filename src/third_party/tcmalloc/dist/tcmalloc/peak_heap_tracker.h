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

#ifndef TCMALLOC_PEAK_HEAP_TRACKER_H_
#define TCMALLOC_PEAK_HEAP_TRACKER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/explicitly_constructed.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/internal/sampled_allocation_recorder.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/sampled_allocation_allocator.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class PeakHeapTracker {
 public:
  constexpr PeakHeapTracker()
      : recorder_lock_(absl::kConstInit,
                       absl::base_internal::SCHEDULE_KERNEL_ONLY),
        peak_heap_recorder_() {}

  void Init(Arena* arena) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock)
      ABSL_LOCKS_EXCLUDED(recorder_lock_) {
    peak_heap_record_allocator_.Init(arena);
    AllocationGuardSpinLockHolder h(&recorder_lock_);
    peak_heap_recorder_.Construct(&peak_heap_record_allocator_);
    peak_heap_recorder_.get_mutable().Init();
  }

  // Possibly save high-water-mark allocation stack traces for peak-heap
  // profile. Should be called immediately after sampling an allocation. If
  // the heap has grown by a sufficient amount since the last high-water-mark,
  // it will save a copy of the sample profile.
  void MaybeSaveSample() ABSL_LOCKS_EXCLUDED(recorder_lock_);

  // Return the saved high-water-mark heap profile, if any.
  std::unique_ptr<ProfileBase> DumpSample() ABSL_LOCKS_EXCLUDED(recorder_lock_);

  size_t CurrentPeakSize() const {
    return do_not_access_directly_peak_sampled_heap_size_.load(
        std::memory_order_relaxed);
  }

 private:
  void SetCurrentPeakSize(int64_t value)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(recorder_lock_) {
    return do_not_access_directly_peak_sampled_heap_size_.store(
        value, std::memory_order_relaxed);
  }

  using PeakHeapRecorder =
      SampleRecorder<SampledAllocation, SampledAllocationAllocator>;

  SampledAllocationAllocator peak_heap_record_allocator_;

  // Guards the peak heap samples stored in `peak_heap_recorder_`.
  absl::base_internal::SpinLock recorder_lock_;

  // Linked list that stores the stack traces of the sampled allocation saved
  // when we allocate memory from the system.
  // PeakHeapRecorder is based off `tcmalloc::tcmalloc_internal::SampleRecorder`
  // , which is mainly used as the allocator and also for iteration here. It
  // reuses memory so we don't have to take the pageheap_lock every time for
  // allocation. `SampleRecorder` has a non-trivial destructor. So wrapping
  // `ExplicitlyConstructed` around it to make the destructor never run.
  ExplicitlyConstructed<PeakHeapRecorder> peak_heap_recorder_
      ABSL_GUARDED_BY(recorder_lock_);

  // Sampled heap size last time peak_heap_recorder_ was saved. Only written
  // under `recorder_lock_`; may be read without it.
  std::atomic<int64_t> do_not_access_directly_peak_sampled_heap_size_{0};

  bool IsNewPeak();
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_PEAK_HEAP_TRACKER_H_
