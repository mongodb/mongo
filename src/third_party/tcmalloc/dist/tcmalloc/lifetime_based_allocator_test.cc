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

#include "tcmalloc/lifetime_based_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <new>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"
#include "absl/synchronization/barrier.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/lifetime_predictions.h"
#include "tcmalloc/internal/lifetime_tracker.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using ::testing::StrEq;

class LifetimeBasedAllocatorTest : public ::testing::Test {
 public:
  // Represents an allocation returned by the lifetime-aware allocator,
  // including any trackers (if applicable).
  struct Allocation {
    LifetimeTracker::Tracker tracker;
    PageId page;
  };

  // Represents memory (and refcount) allocated by the lifetime-aware allocator.
  struct BackingMemory {
    void* ptr;
    int refcount;
  };

  static constexpr absl::Duration kLifetimeThreshold = absl::Milliseconds(100);
  static const int kAllocationSize = kPagesPerHugePage.raw_num() - 2;

  LifetimeBasedAllocatorTest()
      : LifetimeBasedAllocatorTest(LifetimePredictionOptions(
            LifetimePredictionOptions::Mode::kEnabled,
            LifetimePredictionOptions::Strategy::kPredictedLifetimeRegions,
            absl::Milliseconds(500))) {}

  explicit LifetimeBasedAllocatorTest(LifetimePredictionOptions opts)
      : lifetime_allocator_(opts, &region_alloc_, kFakeClock) {}

 protected:
  const Clock kFakeClock =
      Clock{.now = FakeClock, .freq = GetFakeClockFrequency};

  void Advance(absl::Duration d) {
    clock_ += absl::ToDoubleSeconds(d) * GetFakeClockFrequency();
  }

  class FakeRegionAlloc : public LifetimeBasedAllocator::RegionAlloc {
   public:
    explicit FakeRegionAlloc() {
      region_metadata_ =
          BackingMemory{.ptr = calloc(1, sizeof(HugeRegion)), .refcount = 0};
      region_backing_memory_ = BackingMemory{
          .ptr = aligned_alloc(kHugePageSize, HugeRegion::size().in_bytes()),
          .refcount = 0};
    }
    ~FakeRegionAlloc() override {
      free(region_metadata_.ptr);
      free(region_backing_memory_.ptr);
    }

    HugeRegion* AllocRegion(HugeLength n, HugeRange* range) override {
      if (!range->valid()) {
        CHECK_CONDITION(n.in_bytes() == HugeRegion::size().in_bytes());
        CHECK_CONDITION(region_backing_memory_.refcount == 0);
        ++region_backing_memory_.refcount;
        *range =
            HugeRange::Make(HugePageContaining(region_backing_memory_.ptr), n);
      }

      CHECK_CONDITION(region_metadata_.refcount == 0);
      ++region_metadata_.refcount;
      new (region_metadata_.ptr)
          HugeRegion(*range, MemoryModifyFunction(NopUnbackFn));
      return static_cast<HugeRegion*>(region_metadata_.ptr);
    }

   private:
    BackingMemory region_backing_memory_;
    BackingMemory region_metadata_;
  };

  static bool NopUnbackFn(void* p, size_t len) {
    // TODO(b/258278604): Return non-trivial success results.
    return true;
  }

  ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL void Allocate(
      Allocation* out) {
    Length n = Length(kAllocationSize);
    LifetimeStats* context = lifetime_allocator_.CollectLifetimeContext(n);
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    bool from_released;
    auto res = lifetime_allocator_.MaybeGet(n, &from_released, context);
    if (!res.TryGetAllocation(&out->page)) {
      // If not allocated in the short-lived region, do not actually back the
      // memory and return the nullptr span.
      res.InitTracker(&out->tracker);
      lifetime_allocator_.MaybeAddTracker(res, &out->tracker);
      out->page = PageIdContaining(nullptr);
    }
  }

  void Delete(Allocation* alloc) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    lifetime_allocator_.MaybePut(alloc->page, Length(kAllocationSize));
    lifetime_allocator_.MaybePutTracker(&alloc->tracker,
                                        Length(kAllocationSize));
  }

  void GenerateInterestingAllocs() {
    Allocation long_lived_allocs[10];

    for (int i = 0; i < 10; ++i) {
      Allocation short_lived_allocs[10];

      Allocate(&long_lived_allocs[i]);  // will be classified as long-lived

      PRAGMA_NO_UNROLL
      for (int j = 0; j < 10; ++j) {
        Allocate(&short_lived_allocs[j]);  // will be classified as short-lived
      }

      Advance(absl::Microseconds(10));

      for (int j = 0; j < 10; ++j) {
        Delete(&short_lived_allocs[j]);
      }
    }

    Advance(absl::Seconds(10));
    for (int i = 0; i < 10; ++i) {
      Delete(&long_lived_allocs[i]);
    }
  }

  FakeRegionAlloc region_alloc_;
  LifetimeBasedAllocator lifetime_allocator_;

 private:
  static int64_t FakeClock() { return clock_; }

  static double GetFakeClockFrequency() {
    return absl::ToDoubleNanoseconds(absl::Seconds(2));
  }

  static int64_t clock_;
};

int64_t LifetimeBasedAllocatorTest::clock_{0};

class ParameterizedLifetimeBasedAllocatorTest
    : public LifetimeBasedAllocatorTest,
      public ::testing::WithParamInterface<LifetimePredictionOptions> {
 public:
  ParameterizedLifetimeBasedAllocatorTest()
      : LifetimeBasedAllocatorTest(GetParam()) {}
  ~ParameterizedLifetimeBasedAllocatorTest() override {}
};

// Test lifetime predictions.
TEST_P(ParameterizedLifetimeBasedAllocatorTest, Basic) {
  Allocation long_lived_allocs[10];

  for (int i = 0; i < 10; ++i) {
    Allocation short_lived_allocs[10];
    Allocation medium_lived_allocs[10];

    // This pragma is needed since otherwise the compiler may unroll the loop
    // and create more than one allocation site.
    PRAGMA_NO_UNROLL
    for (int j = 0; j < 10; ++j) {
      Allocate(&medium_lived_allocs[j]);  // will be classified as short-lived
    }

    Allocate(&long_lived_allocs[i]);  // will be classified as long-lived

    Advance(absl::Milliseconds(100));

    PRAGMA_NO_UNROLL
    for (int j = 0; j < 10; ++j) {
      Allocate(&short_lived_allocs[j]);  // will be classified as short-lived
    }

    Advance(absl::Microseconds(10));

    for (int j = 0; j < 10; ++j) {
      Delete(&short_lived_allocs[j]);
      Delete(&medium_lived_allocs[j]);
    }
  }

  Advance(absl::Seconds(1));

  LifetimeBasedAllocator::Stats stats = lifetime_allocator_.GetStats();

  EXPECT_EQ(3, stats.database_size);
  EXPECT_EQ(0, stats.database_evictions);

  if (GetParam().always_predict_short_lived()) {
    EXPECT_EQ(210, stats.tracker.short_lived_predictions);
    EXPECT_EQ(0, stats.tracker.long_lived_predictions);
    EXPECT_EQ(110, stats.tracker.expired_lifetimes);
    EXPECT_EQ(0, stats.tracker.overestimated_lifetimes);
    EXPECT_EQ(210, stats.region.allocations);
    EXPECT_EQ(53340, stats.region.allocated_pages);
    EXPECT_EQ(200, stats.region.deallocations);
    EXPECT_EQ(50800, stats.region.deallocated_pages);
  } else {
    EXPECT_EQ(80, stats.tracker.short_lived_predictions);
    EXPECT_EQ(130, stats.tracker.long_lived_predictions);
    EXPECT_EQ(0, stats.tracker.expired_lifetimes);
    EXPECT_EQ(20, stats.tracker.overestimated_lifetimes);
    EXPECT_EQ(80, stats.region.allocations);
    EXPECT_EQ(20320, stats.region.allocated_pages);
    EXPECT_EQ(80, stats.region.deallocations);
    EXPECT_EQ(20320, stats.region.deallocated_pages);
  }

  EXPECT_NE(LifetimePredictionOptions::Mode::kDisabled, stats.opts.mode());
  EXPECT_EQ(absl::Milliseconds(100), stats.opts.threshold());
  EXPECT_EQ(GetParam().counterfactual(), stats.opts.counterfactual());
}

// During allocation, the lifetime-based allocator drops the page heap lock,
// which can lead to complex race conditions. This is a stress-test to uncover
// this behavior.
TEST_P(ParameterizedLifetimeBasedAllocatorTest, MultithreadingStressTest) {
  const int kThreads = 100;
  const int kIterations = 4;
  const int kAllocations = 10;

  for (int i = 0; i < kIterations; ++i) {
    std::vector<std::thread> threads;
    std::vector<Allocation> allocs;
    allocs.resize(kThreads);

    absl::Barrier* b1 = new absl::Barrier(kThreads);
    absl::Barrier* b2 = new absl::Barrier(kThreads);
    for (int j = 0; j < kThreads; ++j) {
      threads.push_back(std::thread([this, j, &b1, &b2, &allocs]() {
        PRAGMA_NO_UNROLL
        for (int k = 0; k < kAllocations; ++k) {
          absl::SleepFor(absl::Microseconds(1));
          Allocate(&allocs[j]);
          // Synchronize the first iteration to ensure that all subsequent
          // iterations get predicted short-lived.
          if (k == 0 && b1->Block()) {
            delete b1;
          }
          absl::SleepFor(absl::Microseconds(1));
          Delete(&allocs[j]);
          if (k == 0 && b2->Block()) {
            delete b2;
          }
        }
      }));
    }

    for (auto& t : threads) {
      t.join();
    }
  }

  // Check that the lifetime prediction works as expected.
  LifetimeBasedAllocator::Stats stats = lifetime_allocator_.GetStats();
  if (GetParam().always_predict_short_lived()) {
    EXPECT_EQ(4000, stats.tracker.short_lived_predictions);
    EXPECT_EQ(0, stats.tracker.long_lived_predictions);
  } else {
    EXPECT_EQ(3900, stats.tracker.short_lived_predictions);
    EXPECT_EQ(100, stats.tracker.long_lived_predictions);
  }
  EXPECT_EQ(1, stats.database_size);
}

TEST_F(LifetimeBasedAllocatorTest, Print) {
  GenerateInterestingAllocs();
  std::string buffer(1024 * 1024, '\0');
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    Printer printer(&*buffer.begin(), buffer.size());
    lifetime_allocator_.Print(&printer);
    buffer.erase(printer.SpaceRequired());
  }

  EXPECT_THAT(buffer, StrEq(R"(HugePageAware: *** Lifetime-based regions: ***
HugePageAware: Predictions: 80 short / 30 long lived (0 expired, 20 overestimated)
HugePageAware: Lifetime-based allocations (enabled / short-lived regions): Threshold = 0.50s, 2 stack traces (0 evicted)
LifetimeBasedRegion: 80 allocated (20320 pages), 80 freed (20320 pages) in lifetime region.

)"));
}

TEST_F(LifetimeBasedAllocatorTest, PrintInPbTxt) {
  GenerateInterestingAllocs();
  std::string buffer(1024 * 1024, '\0');
  Printer printer(&*buffer.begin(), buffer.size());
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    PbtxtRegion region(&printer, kTop);
    lifetime_allocator_.PrintInPbtxt(&region);
  }
  buffer.erase(printer.SpaceRequired());

  auto canonicalize = [](std::string* s) {
    *s = absl::StrReplaceAll(*s, {{"\n", " "}});
    absl::RemoveExtraAsciiWhitespace(s);
  };
  canonicalize(&buffer);
  std::string expected(R"(
  lifetime_based_allocator_stats {
    enabled: true
    counterfactual: false
    threshold_ms: 500
    num_predicted_short_lived: 80
    num_predicted_long_lived: 30
    num_expired: 0
    num_overestimated: 20
    database_size: 2
    database_evicted_count: 0
    lifetime_region_allocated: 80
    lifetime_region_allocated_pages: 20320
    lifetime_region_freed: 80
    lifetime_region_freed_pages: 20320})");
  canonicalize(&expected);
  EXPECT_THAT(buffer, StrEq(expected));
}

INSTANTIATE_TEST_SUITE_P(
    LifetimeTests, ParameterizedLifetimeBasedAllocatorTest,
    testing::Values(
        LifetimePredictionOptions(
            LifetimePredictionOptions::Mode::kEnabled,
            LifetimePredictionOptions::Strategy::kPredictedLifetimeRegions,
            LifetimeBasedAllocatorTest::kLifetimeThreshold),
        LifetimePredictionOptions(
            LifetimePredictionOptions::Mode::kCounterfactual,
            LifetimePredictionOptions::Strategy::kPredictedLifetimeRegions,
            LifetimeBasedAllocatorTest::kLifetimeThreshold),
        LifetimePredictionOptions(
            LifetimePredictionOptions::Mode::kEnabled,
            LifetimePredictionOptions::Strategy::kAlwaysShortLivedRegions,
            LifetimeBasedAllocatorTest::kLifetimeThreshold),
        LifetimePredictionOptions(
            LifetimePredictionOptions::Mode::kCounterfactual,
            LifetimePredictionOptions::Strategy::kAlwaysShortLivedRegions,
            LifetimeBasedAllocatorTest::kLifetimeThreshold)),
    [](const testing::TestParamInfo<LifetimePredictionOptions>& info) {
      return absl::StrFormat(
          "%s_%s", ((info.index >= 2) ? "always_regions" : "lifetime_regions"),
          ((info.index % 2 == 0) ? "enabled" : "counterfactual"));
    });

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
