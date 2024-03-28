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

#include "tcmalloc/stack_trace_table.h"

#include <stddef.h>
#include <string.h>

#include <cstdint>

#include "absl/functional/function_ref.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

StackTraceTable::StackTraceTable(ProfileType type)
    : type_(type), depth_total_(0), all_(nullptr) {}

StackTraceTable::~StackTraceTable() {
  LinkedSample* cur = all_;
  while (cur != nullptr) {
    LinkedSample* next = cur->next;
    cur->~LinkedSample();
    {
      PageHeapSpinLockHolder l;
      tc_globals.linked_sample_allocator().Delete(cur);
    }
    cur = next;
  }
  all_ = nullptr;
}

void StackTraceTable::AddTrace(double sample_weight, const StackTrace& t) {
  depth_total_ += t.depth;
  // Note this makes a copy of the information from the stack trace and users
  // would call TCMalloc public API and iterate over the copied data in the
  // `StackTraceTable`. Ideally, we would want to avoid the copy and let the API
  // iterate over the stack traces directly. However, this would result in
  // deadlocks when users allocate while iterating. For example, allocationz/
  // holds a global lock when calling `AddTrace` and is on the allocation path.
  // New allocations happening under `AddTrace` can be sampled, re-enter the
  // allocation path and cause deadlocks. Another example of deadlock happens
  // when iterating over `tc_globals.sampled_allocation_recorder()` and
  // allocating, see more details in "HeapProfilingTest.AllocateWhileIterating"
  // under google3/tcmalloc/heap_profiling_test.cc.
  LinkedSample* s;
  {
    PageHeapSpinLockHolder l;
    s = tc_globals.linked_sample_allocator().New();
  }
  s = new (s) LinkedSample;

  // Report total bytes that are a multiple of the object size.
  size_t allocated_size = t.allocated_size;
  size_t requested_size = t.requested_size;

  uintptr_t bytes = sample_weight * AllocatedBytes(t) + 0.5;
  // We want sum to be a multiple of allocated_size; pick the nearest
  // multiple rather than always rounding up or down.
  //
  // TODO(b/215362992): Revisit this assertion when GWP-ASan guards
  // zero-byte allocations.
  TC_ASSERT_GT(allocated_size, 0);
  // The reported count of samples, with possible rounding up for unsample.
  s->sample.count = (bytes + allocated_size / 2) / allocated_size;
  s->sample.sum = s->sample.count * allocated_size;
  s->sample.requested_size = requested_size;
  s->sample.requested_alignment = t.requested_alignment;
  s->sample.requested_size_returning = t.requested_size_returning;
  s->sample.allocated_size = allocated_size;
  s->sample.access_hint = static_cast<hot_cold_t>(t.access_hint);
  s->sample.access_allocated = t.cold_allocated ? Profile::Sample::Access::Cold
                                                : Profile::Sample::Access::Hot;
  s->sample.depth = t.depth;
  s->sample.allocation_time = t.allocation_time;

  s->sample.span_start_address = t.span_start_address;
  s->sample.guarded_status =
      static_cast<Profile::Sample::GuardedStatus>(t.guarded_status);

  static_assert(kMaxStackDepth <= Profile::Sample::kMaxStackDepth,
                "Profile stack size smaller than internal stack sizes");
  memcpy(s->sample.stack, t.stack,
         sizeof(s->sample.stack[0]) * s->sample.depth);

  s->next = all_;
  all_ = s;
}

void StackTraceTable::Iterate(
    absl::FunctionRef<void(const Profile::Sample&)> func) const {
  LinkedSample* cur = all_;
  while (cur != nullptr) {
    func(cur->sample);
    cur = cur->next;
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
