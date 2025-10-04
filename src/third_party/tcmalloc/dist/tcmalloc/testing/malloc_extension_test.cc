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
// Test for TCMalloc implementation of MallocExtension

#include "tcmalloc/malloc_extension.h"

#include <stddef.h>

#include <map>
#include <optional>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
class CpuCachePeer {
 public:
  template <typename CpuCache>
  static void ResizeSlab(CpuCache& cpu_cache, bool should_grow) {
    for (int i = 0; i < 100000; ++i)
      cpu_cache.RecordCacheMissStat(/*cpu=*/0, /*is_alloc=*/!should_grow);
    // We need at least one of each type of miss for the ratios to be sensical.
    cpu_cache.RecordCacheMissStat(/*cpu=*/0, /*is_alloc=*/should_grow);
    cpu_cache.ResizeSlabIfNeeded();
  }
};

namespace {

TEST(MallocExtension, BackgroundReleaseRate) {

  // Mutate via MallocExtension.
  MallocExtension::SetBackgroundReleaseRate(
      MallocExtension::BytesPerSecond{100 << 20});

  EXPECT_EQ(static_cast<size_t>(MallocExtension::GetBackgroundReleaseRate()),
            100 << 20);

  // Disable release
  MallocExtension::SetBackgroundReleaseRate(MallocExtension::BytesPerSecond{0});

  EXPECT_EQ(static_cast<size_t>(MallocExtension::GetBackgroundReleaseRate()),
            0);
}

TEST(MallocExtension, SkipSubreleaseIntervals) {

  // Mutate via MallocExtension.
  MallocExtension::SetSkipSubreleaseInterval(absl::Seconds(10));
  EXPECT_EQ(MallocExtension::GetSkipSubreleaseInterval(), absl::Seconds(10));
  MallocExtension::SetSkipSubreleaseShortInterval(absl::Seconds(20));
  EXPECT_EQ(MallocExtension::GetSkipSubreleaseShortInterval(),
            absl::Seconds(20));
  MallocExtension::SetSkipSubreleaseLongInterval(absl::Seconds(30));
  EXPECT_EQ(MallocExtension::GetSkipSubreleaseLongInterval(),
            absl::Seconds(30));

  // Disable skip subrelease by setting all intervals to zero.
  MallocExtension::SetSkipSubreleaseInterval(absl::ZeroDuration());
  EXPECT_EQ(MallocExtension::GetSkipSubreleaseInterval(), absl::ZeroDuration());
  MallocExtension::SetSkipSubreleaseShortInterval(absl::ZeroDuration());
  EXPECT_EQ(MallocExtension::GetSkipSubreleaseShortInterval(),
            absl::ZeroDuration());
  MallocExtension::SetSkipSubreleaseLongInterval(absl::ZeroDuration());
  EXPECT_EQ(MallocExtension::GetSkipSubreleaseLongInterval(),
            absl::ZeroDuration());
}

TEST(MallocExtension, Properties) {
  // Verify that every property under GetProperties also works with
  // GetNumericProperty.
  const auto properties = MallocExtension::GetProperties();
  for (const auto& property : properties) {
    std::optional<size_t> scalar =
        MallocExtension::GetNumericProperty(property.first);
    // The value of the property itself may have changed, so just check that it
    // is present.
    EXPECT_THAT(scalar, testing::Ne(std::nullopt)) << property.first;
  }

  // Test that known GetNumericProperty keys exist under GetProperties.
  constexpr absl::string_view kKnownProperties[] = {
      // clang-format off
      // go/keep-sorted start
      "generic.bytes_in_use_by_app",
      "generic.current_allocated_bytes",
      "generic.heap_size",
      "generic.peak_memory_usage",
      "generic.physical_memory_used",
      "generic.realized_fragmentation",
      "generic.virtual_memory_used",
      "tcmalloc.central_cache_free",
      "tcmalloc.cpu_free",
      "tcmalloc.current_total_thread_cache_bytes",
      "tcmalloc.desired_usage_limit_bytes",
      "tcmalloc.external_fragmentation_bytes",
      "tcmalloc.hard_limit_hits",
      "tcmalloc.hard_usage_limit_bytes",
      "tcmalloc.local_bytes",
      "tcmalloc.max_total_thread_cache_bytes",
      "tcmalloc.metadata_bytes",
      "tcmalloc.page_algorithm",
      "tcmalloc.page_heap_free",
      "tcmalloc.page_heap_unmapped",
      "tcmalloc.pageheap_free_bytes",
      "tcmalloc.pageheap_unmapped_bytes",
      "tcmalloc.per_cpu_caches_active",
      "tcmalloc.required_bytes",
      "tcmalloc.sampled_internal_fragmentation",
      "tcmalloc.sharded_transfer_cache_free",
      "tcmalloc.slack_bytes",
      "tcmalloc.soft_limit_hits",
      "tcmalloc.successful_shrinks_after_hard_limit_hit",
      "tcmalloc.successful_shrinks_after_soft_limit_hit",
      "tcmalloc.thread_cache_count",
      "tcmalloc.thread_cache_free",
      "tcmalloc.transfer_cache_free",
      // go/keep-sorted end
      // clang-format on
  };

  for (const auto& known : kKnownProperties) {
    std::optional<size_t> scalar = MallocExtension::GetNumericProperty(known);
    EXPECT_THAT(scalar, testing::Ne(std::nullopt));
    EXPECT_THAT(properties,
                testing::Contains(testing::Key(testing::Eq(known))));
  }
}

// Test that when we resize the slab repeatedly, the metadata metric is
// positive.
TEST(MallocExtension, DynamicSlabMallocMetadata) {
  if (!tc_globals.CpuCacheActive()) {
    GTEST_SKIP() << "CPU cache disabled.";
  }

  ScopedBackgroundProcessActionsEnabled background(false);

  auto& cpu_cache = tc_globals.cpu_cache();
  for (int i = 0; i < 100; ++i) {
    CpuCachePeer::ResizeSlab(cpu_cache, /*should_grow=*/true);
    CpuCachePeer::ResizeSlab(cpu_cache, /*should_grow=*/false);
  }
  auto properties = MallocExtension::GetProperties();
  EXPECT_THAT(
      properties["tcmalloc.metadata_bytes"],
      testing::Field(&MallocExtension::Property::value, testing::Gt(0)));
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
