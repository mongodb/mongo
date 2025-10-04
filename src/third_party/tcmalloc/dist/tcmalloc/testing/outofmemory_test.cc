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
// Test out of memory handling.  Kept in a separate test since running out
// of memory causes other parts of the runtime to behave improperly.

#include <stddef.h>

#include <new>

#include "gtest/gtest.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

class OutOfMemoryTest : public ::testing::Test {
 public:
  static const size_t kGiB = 1024 * 1024 * 1024;
  OutOfMemoryTest() {
    MallocExtension::SetBackgroundProcessActionsEnabled(false);
    // If memory releaser thread is doing anything at the moment, let it finish.
    absl::SleepFor(absl::Seconds(1));
  }
};

TEST_F(OutOfMemoryTest, TestUntilFailure) {
  ScopedNeverSample never_sample;
  ScopedResourceLimit limit{6 * kGiB};

  // Check that large allocations fail with NULL instead of crashing.
  static const size_t kIncrement = 100 << 20;
  static const size_t kMaxSize = ~static_cast<size_t>(0);
  for (size_t s = kIncrement; s < kMaxSize - kIncrement; s += kIncrement) {
    SCOPED_TRACE(s);
    void* large_object = ::operator new(s, std::nothrow);
    if (large_object == nullptr) {
      return;
    }
    ::operator delete(large_object);
  }
  ASSERT_TRUE(false) << "Did not run out of memory";
}

TEST_F(OutOfMemoryTest, SmallAllocs) {
  ScopedNeverSample never_sample;
  ScopedResourceLimit limit{6 * kGiB};

  // Check that large allocations fail with NULL instead of crashing.
  static constexpr size_t kSize = tcmalloc_internal::kHugePageSize / 2 - 1;
  void* list = nullptr;
  bool found = false;

  for (int i = 0; i < 8000; i++) {
    void* obj = ::operator new(kSize, std::nothrow);
    if (obj == nullptr) {
      found = true;
      break;
    }

    tcmalloc_internal::SLL_Push(&list, obj);
  }

  // Cleanup
  while (list != nullptr) {
    void* obj = tcmalloc_internal::SLL_Pop(&list);
    operator delete(obj);
  }

  ASSERT_TRUE(found) << "Did not run out of memory";
}

}  // namespace
}  // namespace tcmalloc
