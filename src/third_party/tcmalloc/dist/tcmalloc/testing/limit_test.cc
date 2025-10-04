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

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/inlined_vector.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

using tcmalloc_internal::kHugePageSize;
using tcmalloc_internal::kPageSize;
using ::testing::ContainsRegex;
using ::testing::HasSubstr;

void DumpHeapStats(absl::string_view label) {
  std::string buffer = MallocExtension::GetStats();
  absl::FPrintF(stderr, "%s\n%s\n", label, buffer);
}

// Fixture for friend access to MallocExtension.
class LimitTest : public ::testing::Test {
 protected:
  LimitTest() {
    stats_buffer_.reserve(3 << 20);
    stats_pbtxt_.reserve(3 << 20);
  }

  void LimitChangeTriggersReleaseLargeAllocs();
  void LimitChangeTriggersReleaseSmallAllocs();
  void LimitRespected();
  void ExceedingSoftLimitDoesntCrashWithHardLimit();

  void ReleaseMemory() {
    MallocExtension::SetMemoryLimit(0, MallocExtension::LimitKind::kSoft);

    // This will cause a donation that will be immediately subreleased because
    // of the memory limit.
    void* ptr = ::operator new(1544 * 1024);  // Slightly larger than 1.5 MiB
    ::operator delete(ptr);
  }

  size_t GetLimit(MallocExtension::LimitKind limit_kind) {
    return MallocExtension::GetMemoryLimit(limit_kind);
  }

  // avoid fragmentation in local caches
  void* malloc_pages(size_t bytes) {
    TC_CHECK_EQ(bytes % kPageSize, 0);
    void* ptr;
    TC_CHECK_EQ(0, posix_memalign(&ptr, kPageSize, bytes));
    return ptr;
  }

  size_t physical_memory_used() {
    return *MallocExtension::GetNumericProperty("generic.physical_memory_used");
  }

  // Returns a human-readable stats representation.  This is backed by
  // stats_buffer_, to avoid allocating while potentially under a memory limit.
  absl::string_view GetStats() {
    size_t capacity = stats_buffer_.capacity();
    stats_buffer_.resize(capacity);
    char* data = stats_buffer_.data();

    int actual_size = TCMalloc_Internal_GetStats(data, capacity);
    stats_buffer_.erase(actual_size);
    return absl::string_view(data, actual_size);
  }

  // Returns a pbtxt-based stats representation.  This is backed by
  // stats_pbtxt_, to avoid allocating while potentially under a memory limit.
  absl::string_view GetStatsInPbTxt() {
    size_t capacity = stats_pbtxt_.capacity();
    stats_pbtxt_.resize(capacity);
    char* data = stats_pbtxt_.data();

    int actual_size = MallocExtension_Internal_GetStatsInPbtxt(data, capacity);
    stats_pbtxt_.erase(actual_size);
    return absl::string_view(data, actual_size);
  }

  std::string stats_buffer_;
  std::string stats_pbtxt_;
};

using LimitDeathTest = LimitTest;

TEST_F(LimitTest, LimitZeroMeansNoLimit) {
  MallocExtension::SetMemoryLimit(0, MallocExtension::LimitKind::kSoft);
  MallocExtension::SetMemoryLimit(0, MallocExtension::LimitKind::kHard);

  void* ptr = ::operator new(10000);  // Should allocate memory fine.
  ::operator delete(ptr);
}

void LimitTest::LimitRespected() {
  // Needed to see what expectation failed (if any).
  testing::UnitTest::GetInstance()->listeners().SuppressEventForwarding(false);

  static const size_t kLim = 4ul * 1024 * 1024 * 1024;
  MallocExtension::SetMemoryLimit(kLim, MallocExtension::LimitKind::kSoft);

  absl::string_view statsBuf = GetStats();
  absl::string_view statsPbtxt = GetStatsInPbTxt();

  char buf[512];
  absl::SNPrintF(buf, sizeof(buf), "PARAMETER desired_usage_limit_bytes %u",
                 kLim);
  EXPECT_THAT(statsBuf, HasSubstr(buf));
  EXPECT_THAT(statsBuf, HasSubstr("Number of times soft limit was hit: 0"));
  EXPECT_THAT(statsBuf,
              HasSubstr("Number of times memory shrank below soft limit: 0"));
  EXPECT_THAT(statsBuf, HasSubstr("Number of times hard limit was hit: 0"));
  EXPECT_THAT(statsBuf,
              HasSubstr("Number of times memory shrank below hard limit: 0"));

  absl::SNPrintF(buf, sizeof(buf), "desired_usage_limit_bytes: %u", kLim);
  EXPECT_THAT(statsPbtxt, HasSubstr(buf));
  EXPECT_THAT(statsPbtxt, HasSubstr("soft_limit_hits: 0"));
  EXPECT_THAT(statsPbtxt, HasSubstr("hard_limit_hits: 0"));
  EXPECT_THAT(statsPbtxt,
              HasSubstr("successful_shrinks_after_soft_limit_hit: 0"));
  EXPECT_THAT(statsPbtxt,
              HasSubstr("successful_shrinks_after_hard_limit_hit: 0"));

  // Avoid failing due to usage by test itself.
  static const size_t kLimForUse = kLim * 9 / 10;
  // First allocate many small objects...
  size_t used = 0;
  std::vector<void*> ptrs;
  // TODO(b/268541669): Small-but-slow currently has multiple objects per 4K
  // span (kPageSize).  We need to avoid allocating solely kPageSize to prevent
  // CFL fragmentation.
  constexpr size_t kSmallAlloc = 4 * kPageSize;
  while (used < kLimForUse) {
    ptrs.push_back(malloc_pages(kSmallAlloc));
    used += kSmallAlloc;
  }
  DumpHeapStats("after allocating small objects");

  // return much of the space, fragmented...
  bool ret = false;
  for (auto& p : ptrs) {
    if (ret) {
      free(p);
      p = nullptr;
      used -= kSmallAlloc;
    }
    ret = !ret;
  }
  DumpHeapStats("after freeing many small objects");

  // Now ensure we can re use it for large allocations.

  while (used < kLimForUse) {
    const size_t large = kPageSize * 10;
    ptrs.push_back(malloc_pages(large));
    used += large;
  }
  DumpHeapStats("after allocating large objects");
  // We do not track the resident memory exactly, so we add some slack to the
  // limit.
  EXPECT_LE(physical_memory_used(), kLim * 1.2);

  statsBuf = GetStats();
  statsPbtxt = GetStatsInPbTxt();
  // The HugePageAwareAllocator hits the limit more than once.
  EXPECT_THAT(
      statsBuf,
      AllOf(ContainsRegex(R"(Number of times soft limit was hit: [1-9]\d*)"),
            ContainsRegex(R"(Number of times hard limit was hit: 0)")));
  EXPECT_THAT(
      statsBuf,
      AllOf(ContainsRegex(
                R"(Number of times memory shrank below soft limit: [1-9]\d*)"),
            ContainsRegex(
                R"(Number of times memory shrank below hard limit: 0)")));
  EXPECT_THAT(statsPbtxt, ContainsRegex(R"(soft_limit_hits: [1-9]\d*)"));
  EXPECT_THAT(
      statsPbtxt,
      ContainsRegex(R"(successful_shrinks_after_soft_limit_hit: [1-9\d*])"));

  for (auto p : ptrs) {
    free(p);
  }

  // Exit status indicates whether we've failed any of the expectations above.
  exit(testing::Test::HasFailure());
}

TEST_F(LimitTest, LimitRespected) {
  // Run the test in a separate subprocess, so it doesn't interfere with other
  // tests.
  EXPECT_EXIT(LimitRespected(), testing::ExitedWithCode(0), "");
}

// TODO(b/270593199): remove this test after clients have migrated from
// the deprecated MallocExtension::MemoryLimit.
//
// Prior to having separate soft and hard limits, setting a soft limit after
// the hard one removed the hard limit.
// Verify that we maintain this legacy behavior, until the legacy interfaces
// are migrated from.
ABSL_INTERNAL_DISABLE_DEPRECATED_DECLARATION_WARNING
TEST_F(LimitTest, LegacySoftLimitResetsHardLimit) {
  size_t used = physical_memory_used();
  constexpr size_t kMiB = 1 << 20;

  MallocExtension::MemoryLimit limit{.limit = used + kMiB, .hard = true};
  MallocExtension::SetMemoryLimit(limit);
  limit.hard = false;
  MallocExtension::SetMemoryLimit(limit);

  // Only soft limit is in effect, we should be able to allocate 64MiB.
  ::operator delete(::operator new(64 * kMiB));
}
ABSL_INTERNAL_RESTORE_DEPRECATED_DECLARATION_WARNING

TEST_F(LimitTest, HardLimitZeroEqNoLimit) {
  MallocExtension::SetMemoryLimit(0, MallocExtension::LimitKind::kHard);

  // Setting limit to 0 implies no limit. Verify we can still allocate.
  ::operator delete(::operator new(1 << 20));
}

TEST_F(LimitDeathTest, HardLimitRespected) {
  static const size_t kLim = 400 << 20;
  MallocExtension::SetMemoryLimit(kLim, MallocExtension::LimitKind::kHard);

  absl::string_view statsBuf = GetStats();
  absl::string_view statsPbtxt = GetStatsInPbTxt();

  // Avoid gmock matchers, as they require a std::string which may allocate.
  char buf[512];
  absl::SNPrintF(buf, sizeof(buf), "PARAMETER hard_usage_limit_bytes %u", kLim);
  EXPECT_TRUE(absl::StrContains(statsBuf, buf)) << statsBuf;

  absl::SNPrintF(buf, sizeof(buf), "hard_usage_limit_bytes: %u", kLim);
  EXPECT_TRUE(absl::StrContains(statsPbtxt, buf)) << statsPbtxt;

  ASSERT_DEATH(malloc_pages(400 << 20), "limit");

  MallocExtension::SetMemoryLimit(std::numeric_limits<size_t>::max(),
                                  MallocExtension::LimitKind::kHard);
}

TEST_F(LimitDeathTest, HardLimitRespectsNoSubrelease) {
  static const size_t kLim = 300 << 20;
  MallocExtension::SetMemoryLimit(kLim, MallocExtension::LimitKind::kHard);
  TCMalloc_Internal_SetHPAASubrelease(false);
  EXPECT_FALSE(TCMalloc_Internal_GetHPAASubrelease());

  absl::string_view statsBuf = GetStats();
  absl::string_view statsPbtxt = GetStatsInPbTxt();

  char buf[512];
  absl::SNPrintF(buf, sizeof(buf), "PARAMETER hard_usage_limit_bytes %u", kLim);
  EXPECT_THAT(statsBuf, HasSubstr(buf));

  absl::SNPrintF(buf, sizeof(buf), "hard_usage_limit_bytes: %u", kLim);
  EXPECT_THAT(statsPbtxt, HasSubstr(buf));

  ASSERT_DEATH(
      []() {
        // Allocate a bunch of medium objects, free half of them to cause some
        // fragmentation, then allocate some large objects. If we subrelease we
        // could stay under our hard limit, but if we don't then we should go
        // over.
        std::vector<void*> ptrs;
        constexpr size_t kNumMediumObjs = 400;
        constexpr size_t kNumLargeObjs = 200;
        for (size_t i = 0; i < kNumMediumObjs; i++) {
          ptrs.push_back(::operator new(512 << 10));
        }
        DumpHeapStats("after allocating medium objects");
        for (size_t i = 0; i < ptrs.size(); i++) {
          if (i % 2) continue;
          ::operator delete(ptrs[i]);
          ptrs[i] = static_cast<void*>(nullptr);
        }
        DumpHeapStats("after freeing half of medium objects");
        for (size_t i = 0; i < kNumLargeObjs; i++) {
          ptrs.push_back(::operator new(1 << 20));
        }
        DumpHeapStats("after allocating large objects");
        while (!ptrs.empty()) {
          ::operator delete(ptrs.back());
          ptrs.pop_back();
        }
        DumpHeapStats("after freeing all objects");
      }(),
      "limit");
  MallocExtension::SetMemoryLimit(std::numeric_limits<size_t>::max(),
                                  MallocExtension::LimitKind::kHard);
}

// Tests interactions between the lifetime-based allocator and memory limits.
// Since memory limits are global for the entire test binary, we need to run
// this against the main allocator instead of our own instance.
TEST_F(LimitTest, LifetimeAllocatorPath) {
  // Enable subrelease and cause the allocator to eagerly release all memory.
  bool previous_subrelease = TCMalloc_Internal_GetHPAASubrelease();
  TCMalloc_Internal_SetHPAASubrelease(true);
  EXPECT_TRUE(TCMalloc_Internal_GetHPAASubrelease());
  MallocExtension::SetMemoryLimit(0, MallocExtension::LimitKind::kSoft);

  // This will cause a donation that will be immediately subreleased because of
  // the memory limit. This catches a problem in the allocation path where the
  // allocator assumed that donated allocations will stay donated until the
  // allocation has finished.
  void* ptr = ::operator new(1544 * 1024);  // Slightly larger than 1.5 MiB
  ::operator delete(ptr);

  TCMalloc_Internal_SetHPAASubrelease(previous_subrelease);
}

void LimitTest::LimitChangeTriggersReleaseSmallAllocs() {
  // Needed to see what expectation failed (if any).
  testing::UnitTest::GetInstance()->listeners().SuppressEventForwarding(false);

  // Verify that changing the limit to below the current page heap size causes
  // memory to be released to the extent possible.

  constexpr size_t kSize = 1 << 30;
  constexpr size_t kAllocSize = tcmalloc_internal::kHugePageSize / 4;
  absl::flat_hash_map<uintptr_t, absl::InlinedVector<void*, 4>> pointers;
  pointers.reserve(kSize / kAllocSize);

  const size_t heap_size =
      *MallocExtension::GetNumericProperty("generic.heap_size");

  // Trigger many allocations that will rest in the page heap momentarily.  We
  // alternate between allocations retained/deallocated to fragment the
  // HugePageFiller.
  for (size_t allocated = 0; allocated < kSize; allocated += kAllocSize) {
    void* alloc = ::operator new(kAllocSize);

    constexpr size_t kHugePageMask = ~(kHugePageSize - 1u);
    pointers[reinterpret_cast<uintptr_t>(alloc) & kHugePageMask].push_back(
        alloc);
  }

  // Under HPAA, we expect to have colocated allocations on the same huge page
  // frequently.  We drain all-but-one allocation from each huge page.
  size_t dropped = 0;
  for (auto& [k, v] : pointers) {
    for (size_t i = 1; i < v.size(); ++i) {
      ::operator delete(v[i]);
    }
    dropped += v.size() - 1;
    v.resize(1);
  }

  const size_t old_free =
      *MallocExtension::GetNumericProperty("tcmalloc.pageheap_free_bytes");
  const size_t old_unmapped =
      *MallocExtension::GetNumericProperty("tcmalloc.pageheap_unmapped_bytes");
  // The page heap may be successful at coalescing some allocations, so we aim
  // for a lowerbound of kSize/2.  This is still separated from the upperbound
  // after changing the limit of 3*kSize/8.
  EXPECT_GE(old_free, kSize / 2);

  // Change limit.
  MallocExtension::SetMemoryLimit(heap_size + kSize / 2,
                                  MallocExtension::LimitKind::kSoft);

  const size_t new_free =
      *MallocExtension::GetNumericProperty("tcmalloc.pageheap_free_bytes");
  const size_t new_unmapped =
      *MallocExtension::GetNumericProperty("tcmalloc.pageheap_unmapped_bytes");

  // We use a lower bound of ~1/2 of the ~3/4 actually moving, since background
  // allocations can throw off our stats and fragmentation is not guaranteed.
  EXPECT_LT(new_free, 3 * kSize / 8);
  EXPECT_GE(new_unmapped,
            old_unmapped + (old_free > new_free ? old_free - new_free : 0) / 2)
      << new_unmapped << " " << old_unmapped << " " << new_free << " "
      << old_free;

  // Expect that we dropped at least half of the allocations.
  EXPECT_GE(dropped, kSize / kAllocSize / 2);

  // Exit status indicates whether we've failed any of the expectations above.
  exit(testing::Test::HasFailure());
}

TEST_F(LimitTest, LimitChangeTriggersReleaseSmallAllocs) {
  // Run the test in a separate subprocess, so it doesn't interfere with other
  // tests.
  EXPECT_EXIT(LimitChangeTriggersReleaseSmallAllocs(),
              testing::ExitedWithCode(0), "");
}

void LimitTest::LimitChangeTriggersReleaseLargeAllocs() {
  // Needed to see what expectation failed (if any).
  testing::UnitTest::GetInstance()->listeners().SuppressEventForwarding(false);

  // Verify that changing the limit to below the current page heap size causes
  // memory to be released to the extent possible.

  constexpr size_t kSize = 1 << 30;

  const size_t heap_size =
      *MallocExtension::GetNumericProperty("generic.heap_size");

  // Trigger a large allocation that will rest in the page heap momentarily.
  ::operator delete(::operator new(kSize));

  const size_t old_free =
      *MallocExtension::GetNumericProperty("tcmalloc.pageheap_free_bytes");
  const size_t old_unmapped =
      *MallocExtension::GetNumericProperty("tcmalloc.pageheap_unmapped_bytes");

  // Change limit.
  MallocExtension::SetMemoryLimit(heap_size + kSize / 2,
                                  MallocExtension::LimitKind::kSoft);

  const size_t new_free =
      *MallocExtension::GetNumericProperty("tcmalloc.pageheap_free_bytes");
  const size_t new_unmapped =
      *MallocExtension::GetNumericProperty("tcmalloc.pageheap_unmapped_bytes");

  EXPECT_LT(new_free, kSize / 2);
  EXPECT_GE(new_unmapped,
            old_unmapped + (old_free > new_free ? old_free - new_free : 0) / 2)
      << new_unmapped << " " << old_unmapped << " " << new_free << " "
      << old_free;
  // Exit status indicates whether we've failed any of the expectations above.
  exit(testing::Test::HasFailure());
}

TEST_F(LimitTest, LimitChangeTriggersReleaseLargeAllocs) {
  // Run the test in a separate subprocess, so it doesn't interfere with other
  // tests.
  EXPECT_EXIT(LimitChangeTriggersReleaseLargeAllocs(),
              testing::ExitedWithCode(0), "");
}

void LimitTest::ExceedingSoftLimitDoesntCrashWithHardLimit() {
  const size_t heap_size =
      *MallocExtension::GetNumericProperty("generic.heap_size");

  const size_t soft_limit = heap_size + (1 << 20);
  const size_t hard_limit = soft_limit + (100 << 20);
  MallocExtension::SetMemoryLimit(soft_limit,
                                  MallocExtension::LimitKind::kSoft);
  MallocExtension::SetMemoryLimit(hard_limit,
                                  MallocExtension::LimitKind::kHard);

  // Allocate 50MiB -- well above soft limit, but well below hard one.
  // We should not crash.
  ::operator delete(::operator new(50 << 20));

  // Exit status indicates whether we've failed any of the expectations above.
  exit(testing::Test::HasFailure());
}

TEST_F(LimitTest, ExceedingSoftLimitDoesntCrashWithHardLimit) {
  // Run the test in a separate subprocess, so it doesn't interfere with other
  // tests.
  EXPECT_EXIT(ExceedingSoftLimitDoesntCrashWithHardLimit(),
              testing::ExitedWithCode(0), "");
}

}  // namespace
}  // namespace tcmalloc
