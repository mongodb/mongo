// Copyright 2022 The TCMalloc Authors
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

#ifndef TCMALLOC_ALLOCATION_SAMPLE_H_
#define TCMALLOC_ALLOCATION_SAMPLE_H_

#include <memory>

#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/stack_trace_table.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

class AllocationSampleList;

class AllocationSample final : public AllocationProfilingTokenBase {
 public:
  AllocationSample(AllocationSampleList* list, absl::Time start);
  ~AllocationSample() override;

  Profile Stop() && override;

 private:
  AllocationSampleList* list_;
  std::unique_ptr<StackTraceTable> mallocs_;
  absl::Time start_;
  AllocationSample* next_ = nullptr;
  friend class AllocationSampleList;
};

class AllocationSampleList {
 public:
  constexpr AllocationSampleList() = default;

  void Add(AllocationSample* as) {
    AllocationGuardSpinLockHolder h(&lock_);
    as->next_ = first_;
    first_ = as;
  }

  // This list is very short and we're nowhere near a hot path, just walk
  void Remove(AllocationSample* as) {
    AllocationGuardSpinLockHolder h(&lock_);
    AllocationSample** link = &first_;
    AllocationSample* cur = first_;
    while (cur != as) {
      TC_CHECK_NE(cur, nullptr);
      link = &cur->next_;
      cur = cur->next_;
    }
    *link = as->next_;
  }

  void ReportMalloc(const struct StackTrace& sample) {
    AllocationGuardSpinLockHolder h(&lock_);
    AllocationSample* cur = first_;
    while (cur != nullptr) {
      cur->mallocs_->AddTrace(1.0, sample);
      cur = cur->next_;
    }
  }

 private:
  // Guard against any concurrent modifications on the list of allocation
  // samples. Invoking `new` while holding this lock can lead to deadlock.
  absl::base_internal::SpinLock lock_{
      absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY};
  AllocationSample* first_ ABSL_GUARDED_BY(lock_) = nullptr;
};

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_ALLOCATION_SAMPLE_H_
