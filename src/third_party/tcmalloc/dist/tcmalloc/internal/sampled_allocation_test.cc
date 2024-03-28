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

#include "tcmalloc/internal/sampled_allocation.h"

#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/debugging/stacktrace.h"
#include "tcmalloc/internal/logging.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

StackTrace PrepareStackTrace() {
  StackTrace st;
  st.depth = absl::GetStackTrace(st.stack, kMaxStackDepth, /* skip_count= */ 0);
  st.requested_size = 8;
  st.requested_alignment = 4;
  st.allocated_size = 8;
  st.access_hint = 1;
  st.weight = 4;
  return st;
}

TEST(SampledAllocationTest, PrepareForSampling) {
  // PrepareForSampling() invoked in the constructor.
  SampledAllocation sampled_allocation(PrepareStackTrace());
  absl::base_internal::SpinLockHolder sample_lock(&sampled_allocation.lock);

  // Now verify some fields.
  EXPECT_GT(sampled_allocation.sampled_stack.depth, 0);
  EXPECT_EQ(sampled_allocation.sampled_stack.requested_size, 8);
  EXPECT_EQ(sampled_allocation.sampled_stack.requested_alignment, 4);
  EXPECT_EQ(sampled_allocation.sampled_stack.allocated_size, 8);
  EXPECT_EQ(sampled_allocation.sampled_stack.access_hint, 1);
  EXPECT_EQ(sampled_allocation.sampled_stack.weight, 4);

  // Set them to different values.
  sampled_allocation.sampled_stack.depth = 0;
  sampled_allocation.sampled_stack.requested_size = 0;
  sampled_allocation.sampled_stack.requested_alignment = 0;
  sampled_allocation.sampled_stack.allocated_size = 0;
  sampled_allocation.sampled_stack.access_hint = 0;
  sampled_allocation.sampled_stack.weight = 0;

  // Call PrepareForSampling() again and check the fields.
  sampled_allocation.PrepareForSampling(PrepareStackTrace());
  EXPECT_GT(sampled_allocation.sampled_stack.depth, 0);
  EXPECT_EQ(sampled_allocation.sampled_stack.requested_size, 8);
  EXPECT_EQ(sampled_allocation.sampled_stack.requested_alignment, 4);
  EXPECT_EQ(sampled_allocation.sampled_stack.allocated_size, 8);
  EXPECT_EQ(sampled_allocation.sampled_stack.access_hint, 1);
  EXPECT_EQ(sampled_allocation.sampled_stack.weight, 4);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
