// Copyright 2021 The TCMalloc Authors
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

#ifndef TCMALLOC_INTERNAL_SAMPLED_ALLOCATION_H_
#define TCMALLOC_INTERNAL_SAMPLED_ALLOCATION_H_

#include <utility>

#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sampled_allocation_recorder.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Stores information about the sampled allocation.
struct SampledAllocation : public tcmalloc_internal::Sample<SampledAllocation> {
  // We use this constructor to initialize `graveyard_`, which is used to
  // maintain the freelist of SampledAllocations. When we revive objects from
  // the freelist, we use `PrepareForSampling()` to update the state of the
  // object.
  constexpr SampledAllocation() = default;

  // When no object is available on the freelist, we allocate for a new
  // SampledAllocation object and invoke this constructor with
  // `PrepareForSampling()`.
  explicit SampledAllocation(StackTrace&& stack_trace) {
    PrepareForSampling(std::move(stack_trace));
  }

  SampledAllocation(const SampledAllocation&) = delete;
  SampledAllocation& operator=(const SampledAllocation&) = delete;

  SampledAllocation(SampledAllocation&&) = delete;
  SampledAllocation& operator=(SampledAllocation&&) = delete;

  // Prepares the state of the object. It is invoked when either a new sampled
  // allocation is constructed or when an object is revived from the freelist.
  void PrepareForSampling(StackTrace&& stack_trace)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock) {
    sampled_stack = std::move(stack_trace);
  }

  // The stack trace of the sampled allocation.
  StackTrace sampled_stack = {};
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_SAMPLED_ALLOCATION_H_
