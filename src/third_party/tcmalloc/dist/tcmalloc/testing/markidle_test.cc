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
// MallocExtension::MarkThreadIdle() testing
#include <stddef.h>
#include <stdlib.h>

#include <optional>
#include <thread>  // NOLINT(build/c++11)

#include "gtest/gtest.h"
#include "absl/types/optional.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

// Helper routine to do lots of allocations
static void TestAllocation() {
  static const int kNum = 1000;
  void* ptr[kNum];
  for (int size = 8; size <= 65536; size *= 2) {
    for (int i = 0; i < kNum; i++) {
      ptr[i] = malloc(size);
    }
    for (int i = 0; i < kNum; i++) {
      free(ptr[i]);
    }
  }
}

// Routine that does a bunch of MarkThreadIdle() calls in sequence
// without any intervening allocations
TEST(MarkIdleTest, MultipleIdleCalls) {
  std::thread t([]() {
    for (int i = 0; i < 4; i++) {
      MallocExtension::MarkThreadIdle();
    }
  });

  t.join();
}

// Routine that does a bunch of MarkThreadIdle() calls in sequence
// with intervening allocations
TEST(MarkIdleTest, MultipleIdleNonIdlePhases) {
  std::thread t([]() {
    for (int i = 0; i < 4; i++) {
      TestAllocation();
      MallocExtension::MarkThreadIdle();
    }
  });

  t.join();
}

// Get current thread cache usage
static size_t GetTotalThreadCacheSize() {
  std::optional<size_t> result = MallocExtension::GetNumericProperty(
      "tcmalloc.current_total_thread_cache_bytes");
  EXPECT_TRUE(result.has_value());
  return *result;
}

// Check that MarkThreadIdle() actually reduces the amount
// of per-thread memory.
TEST(MarkIdleTest, TestIdleUsage) {
  std::thread t([]() {
    const size_t original = GetTotalThreadCacheSize();

    TestAllocation();
    const size_t post_allocation = GetTotalThreadCacheSize();
    ASSERT_GE(post_allocation, original);

    MallocExtension::MarkThreadIdle();
    const size_t post_idle = GetTotalThreadCacheSize();
    ASSERT_LE(post_idle, original);
  });

  t.join();
}

}  // namespace
}  // namespace tcmalloc
