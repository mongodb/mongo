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

#include "tcmalloc/internal/cache_topology.h"

#include <sched.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

TEST(CacheTopology, ComputesSomethingReasonable) {
  // This test verifies that each L3 cache serves the same number of CPU. This
  // is not a strict requirement for the correct operation of this code, but a
  // sign of sanity.
  uint8_t l3_cache_index[CPU_SETSIZE];
  const int num_nodes =
      tcmalloc::tcmalloc_internal::BuildCpuToL3CacheMap(l3_cache_index);
  EXPECT_EQ(absl::base_internal::NumCPUs() % num_nodes, 0);
  ASSERT_GT(num_nodes, 0);
  static const int kMaxNodes = 256 / 8;
  int count_per_node[kMaxNodes] = {0};
  for (int i = 0; i < absl::base_internal::NumCPUs(); ++i) {
    count_per_node[l3_cache_index[i]]++;
  }
  for (int i = 0; i < num_nodes; ++i) {
    EXPECT_EQ(count_per_node[i], absl::base_internal::NumCPUs() / num_nodes);
  }
}

TEST(CacheTopology, FindFirstNumberInBuf) {
  using tcmalloc::tcmalloc_internal::BuildCpuToL3CacheMap_FindFirstNumberInBuf;
  EXPECT_EQ(7, BuildCpuToL3CacheMap_FindFirstNumberInBuf("7,-787"));
  EXPECT_EQ(5, BuildCpuToL3CacheMap_FindFirstNumberInBuf("5"));
  EXPECT_EQ(5, BuildCpuToL3CacheMap_FindFirstNumberInBuf("5-9"));
  EXPECT_EQ(5, BuildCpuToL3CacheMap_FindFirstNumberInBuf("5,9"));
}

}  // namespace
