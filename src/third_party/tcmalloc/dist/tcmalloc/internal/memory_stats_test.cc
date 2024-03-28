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

#include "tcmalloc/internal/memory_stats.h"

#include <stdint.h>
#include <unistd.h>


#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(Stats, ValidRanges) {
  MemoryStats stats;
#if defined(__linux__)
  ASSERT_TRUE(GetMemoryStats(&stats));
#else
  ASSERT_FALSE(GetMemoryStats(&stats));
  return;
#endif

  EXPECT_GT(stats.vss, 0);
  EXPECT_GT(stats.rss, 0);
  EXPECT_GT(stats.shared, 0);
  EXPECT_GT(stats.code, 0);
  EXPECT_GT(stats.data, 0);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
