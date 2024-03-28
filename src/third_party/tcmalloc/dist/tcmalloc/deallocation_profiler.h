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

#ifndef TCMALLOC_DEALLOCATION_PROFILER_H_
#define TCMALLOC_DEALLOCATION_PROFILER_H_

#include <memory>

#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace deallocationz {

class DeallocationProfiler;

class DeallocationProfilerList {
 public:
  constexpr DeallocationProfilerList() = default;

  void ReportMalloc(const tcmalloc_internal::StackTrace& stack_trace);
  void ReportFree(tcmalloc_internal::AllocHandle handle);
  void Add(DeallocationProfiler* profiler);
  void Remove(DeallocationProfiler* profiler);

 private:
  DeallocationProfiler* first_ = nullptr;
  absl::base_internal::SpinLock profilers_lock_{
      absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY};
};

class DeallocationSample final
    : public tcmalloc_internal::AllocationProfilingTokenBase {
 public:
  explicit DeallocationSample(DeallocationProfilerList* list);
  // We define the dtor to ensure it is placed in the desired text section.
  ~DeallocationSample() override = default;

  tcmalloc::Profile Stop() && override;

 private:
  std::unique_ptr<DeallocationProfiler> profiler_;
};

namespace internal {
absl::Duration LifetimeNsToBucketedDuration(double lifetime_ns);
}  // namespace internal
}  // namespace deallocationz
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_DEALLOCATION_PROFILER_H_
