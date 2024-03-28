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
//
// Test speed of handling fragmented heap

#include <stddef.h>

#include <new>
#include <vector>

#include "gtest/gtest.h"
#include "tcmalloc/common.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

TEST(Fragmentation, Slack) {
  // Make kAllocSize one page larger than the maximum small object size.
  static const int kAllocSize =
      tcmalloc_internal::kMaxSize + tcmalloc_internal::kPageSize;
  // Allocate 400MB in total.
  static const int kTotalAlloc = 400 << 20;
  static const int kAllocIterations = kTotalAlloc / kAllocSize;

  // Allocate lots of objects
  std::vector<void*> saved(kAllocIterations);
  for (int i = 0; i < kAllocIterations; i++) {
    saved[i] = ::operator new(kAllocSize);
  }

  // Check the current "slack".
  size_t slack_before =
      *MallocExtension::GetNumericProperty("tcmalloc.slack_bytes");

  // Free alternating ones to fragment heap
  size_t free_bytes = 0;
  for (int i = 0; i < saved.size(); i += 2) {
    ::operator delete(saved[i]);
    free_bytes += kAllocSize;
  }

  // Check that slack delta is within 10% of expected.
  size_t slack_after =
      *MallocExtension::GetNumericProperty("tcmalloc.slack_bytes");
  ASSERT_GE(slack_after, slack_before);
  size_t slack = slack_after - slack_before;

  EXPECT_GT(double(slack), 0.9 * free_bytes);
  EXPECT_LT(double(slack), 1.1 * free_bytes);

  // Free remaining allocations.
  for (int i = 1; i < saved.size(); i += 2) {
    ::operator delete(saved[i]);
  }
}

}  // namespace
}  // namespace tcmalloc
