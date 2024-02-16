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

#include "tcmalloc/sampled_allocation_allocator.h"

#include "gtest/gtest.h"
#include "absl/debugging/stacktrace.h"
#include "tcmalloc/internal/logging.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(SampledAllocationAllocatorTest, AllocAndDealloc) {
  Arena arena;
  SampledAllocationAllocator allocator;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    allocator.Init(&arena);
  }
  StackTrace st;
  st.depth = absl::GetStackTrace(st.stack, kMaxStackDepth, /* skip_count= */ 0);
  st.requested_size = 8;
  st.allocated_size = 8;
  SampledAllocation* sampled_allocation = allocator.New(st);
  EXPECT_GT(sampled_allocation->sampled_stack.depth, 0);
  EXPECT_EQ(sampled_allocation->sampled_stack.requested_size, 8);
  EXPECT_EQ(sampled_allocation->sampled_stack.allocated_size, 8);
  allocator.Delete(sampled_allocation);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
