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

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <new>
#include <optional>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/flags/flag.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/barrier.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/lifetime_tracker.h"
#include "tcmalloc/lifetime_based_allocator.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/testing/testutil.h"

ABSL_FLAG(bool, run_all_tests, false,
          "If true, runs long-running stress tests that are disabled for "
          "regular test runs.");

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

constexpr absl::Duration kLifetimeThreshold = absl::Milliseconds(500);
constexpr absl::Duration kWaitLong = absl::Milliseconds(510);
constexpr absl::Duration kWaitShort = absl::Milliseconds(1);

std::string GetTestName(LifetimePredictionOptions opts) {
  if (opts.always_predict_short_lived()) {
    return opts.enabled() ? "enabled_always" : "counterfactual_always";
  } else {
    return opts.enabled() ? "enabled" : "counterfactual";
  }
}

const LifetimePredictionOptions kTestConfigurations[] = {
    LifetimePredictionOptions(
        LifetimePredictionOptions::Mode::kEnabled,
        LifetimePredictionOptions::Strategy::kPredictedLifetimeRegions,
        kLifetimeThreshold),
    LifetimePredictionOptions(
        LifetimePredictionOptions::Mode::kCounterfactual,
        LifetimePredictionOptions::Strategy::kPredictedLifetimeRegions,
        kLifetimeThreshold),
    LifetimePredictionOptions(
        LifetimePredictionOptions::Mode::kEnabled,
        LifetimePredictionOptions::Strategy::kAlwaysShortLivedRegions,
        kLifetimeThreshold),
    LifetimePredictionOptions(
        LifetimePredictionOptions::Mode::kCounterfactual,
        LifetimePredictionOptions::Strategy::kAlwaysShortLivedRegions,
        kLifetimeThreshold)};

// The purpose of this test is to provide a more complete, end-to-end test for
// lifetime-based allocation, including real timings and the full allocator.
class LifetimesTest
    : public ::testing::TestWithParam<LifetimePredictionOptions> {
 public:
  explicit LifetimesTest() {
    // HugePageAwareAllocator can't be destroyed cleanly, so we store a pointer
    // to one and construct in place.
    void* p = malloc(sizeof(HugePageAwareAllocator));
    allocator_ = new (p) HugePageAwareAllocator(
        MemoryTag::kNormal, HugeRegionCountOption::kSlack, GetParam());
    lifetime_allocator_ = &allocator_->lifetime_based_allocator();

    // Warm up the allocator so that objects aren't placed in (regular) regions.
    for (int i = 0; i < 10000; ++i) {
      allocator_->New(kPagesPerHugePage / 2 - Length(1), 1);
    }
  }

  // Functions for generating different allocation sites with distinct stack
  // traces (A/B). Inlining and tail call optimizations need to be disabled to
  // ensure that the stack traces are guaranteed to be distinct.
  ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL Span* AllocateA() {
    return allocator_->New(Length(kAllocationSize), 1);
  }

  ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL Span* AllocateB() {
    return allocator_->New(Length(kAllocationSize), 1);
  }

  // Generates distinct allocation stack traces based on the value of id.
  ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL Span*
  AllocateWithStacktraceId(int id) {
    if (id == 0) {
      return allocator_->New(Length(kAllocationSize), 1);
    } else if (id % 2 == 0) {
      return AllocateWithStacktraceId(id / 2);
    } else {
      return AllocateWithStacktraceId_2(id / 2);
    }
  }

  void Delete(Span* span) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    allocator_->Delete(span, 1);
  }

 protected:
  const int kAllocationSize = kPagesPerHugePage.raw_num() - 2;

  HugePageAwareAllocator* allocator_;
  LifetimeBasedAllocator* lifetime_allocator_;

  BackingStats GetAllocatorStats() {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    return allocator_->stats();
  }

  int64_t GetUsedBytes(BackingStats s) {
    return s.system_bytes - s.free_bytes - s.unmapped_bytes;
  }

 private:
  ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NO_TAIL_CALL Span*
  AllocateWithStacktraceId_2(int id) {
    if (id == 0) {
      return allocator_->New(Length(kAllocationSize), 1);
    } else if (id % 2 == 0) {
      return AllocateWithStacktraceId(id / 2);
    } else {
      return AllocateWithStacktraceId_2(id / 2);
    }
  }
};

// Tests lifetime predictions.
TEST_P(LifetimesTest, Basic) {
  Span* long_lived_allocs[10];

  for (int i = 0; i < 10; ++i) {
    Span* short_lived_allocs[10];
    Span* shorter_lived_allocs[10];

    PRAGMA_NO_UNROLL
    for (int j = 0; j < 10; ++j) {
      short_lived_allocs[j] = AllocateA();  // medium-lived
    }

    long_lived_allocs[i] = AllocateB();  // long-lived

    absl::SleepFor(kWaitLong);

    PRAGMA_NO_UNROLL
    for (int j = 0; j < 10; ++j) {
      shorter_lived_allocs[j] = AllocateA();  // short-lived
    }

    absl::SleepFor(kWaitShort);

    for (int j = 0; j < 10; ++j) {
      Delete(short_lived_allocs[j]);
      Delete(shorter_lived_allocs[j]);
    }
  }

  for (int i = 0; i < 10; ++i) {
    Delete(long_lived_allocs[i]);
  }

  LifetimeBasedAllocator::Stats stats = lifetime_allocator_->GetStats();

  EXPECT_EQ(3, stats.database_size);
  EXPECT_EQ(0, stats.database_evictions);

  if (GetParam().always_predict_short_lived()) {
    EXPECT_EQ(210, stats.tracker.short_lived_predictions);
    EXPECT_EQ(0, stats.tracker.long_lived_predictions);
    EXPECT_EQ(110, stats.tracker.expired_lifetimes);
    EXPECT_EQ(0, stats.tracker.overestimated_lifetimes);
    EXPECT_EQ(210, stats.region.allocations);
    EXPECT_EQ(53340, stats.region.allocated_pages);
    EXPECT_EQ(210, stats.region.deallocations);
    EXPECT_EQ(53340, stats.region.deallocated_pages);
  } else {
    // The first 20 short-lived allocations are not allocated in the short-lived
    // region because the lifetime-based allocator does not yet have enough
    // profiling data to place them there. Subsequent iterations are placed in
    // the short-lived region.
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
  EXPECT_EQ(kLifetimeThreshold, stats.opts.threshold());
  EXPECT_EQ(GetParam().counterfactual(), stats.opts.counterfactual());
}

// Inspects the state of the lifetime-based region when non-empty.
TEST_P(LifetimesTest, IntermediateState) {
  const int kAllocations = 100;
  const int kIterations = 10;

  Span* short_lived_allocs[kAllocations];

  BackingStats initial_stats;

  PRAGMA_NO_UNROLL
  for (int i = 0; i < kIterations; ++i) {
    PRAGMA_NO_UNROLL
    for (int j = 0; j < kAllocations; ++j) {
      short_lived_allocs[j] = AllocateA();  // short-lived
    }

    // Don't delete the last batch of allocations, as we are testing the
    // allocator's state while holding live objects.
    if (i == kIterations - 1) {
      break;
    }

    for (int j = 0; j < kAllocations; ++j) {
      Delete(short_lived_allocs[j]);
    }

    // Once we have reached the second iteration, everything should be allocated
    // in the short-lived region. Record the stats at this point to capture
    // whether the difference in memory usage is correct.
    if (i == 0) {
      initial_stats = GetAllocatorStats();
    }
  }

  // Check the intermediate state.
  LifetimeBasedAllocator::Stats stats = lifetime_allocator_->GetStats();

  EXPECT_EQ(1, stats.database_size);
  EXPECT_EQ(0, stats.database_evictions);

  if (GetParam().always_predict_short_lived()) {
    EXPECT_EQ(1000, stats.tracker.short_lived_predictions);
    EXPECT_EQ(0, stats.tracker.long_lived_predictions);
    EXPECT_EQ(0, stats.tracker.expired_lifetimes);
    EXPECT_EQ(0, stats.tracker.overestimated_lifetimes);
    EXPECT_EQ(1000, stats.region.allocations);
    EXPECT_EQ(254000, stats.region.allocated_pages);
    EXPECT_EQ(900, stats.region.deallocations);
    EXPECT_EQ(228600, stats.region.deallocated_pages);
  } else {
    EXPECT_EQ(900, stats.tracker.short_lived_predictions);
    EXPECT_EQ(100, stats.tracker.long_lived_predictions);
    EXPECT_EQ(0, stats.tracker.expired_lifetimes);
    EXPECT_EQ(100, stats.tracker.overestimated_lifetimes);
    EXPECT_EQ(900, stats.region.allocations);
    EXPECT_EQ(228600, stats.region.allocated_pages);
    EXPECT_EQ(800, stats.region.deallocations);
    EXPECT_EQ(203200, stats.region.deallocated_pages);
  }

  EXPECT_NE(LifetimePredictionOptions::Mode::kDisabled, stats.opts.mode());
  EXPECT_EQ(kLifetimeThreshold, stats.opts.threshold());
  EXPECT_EQ(GetParam().counterfactual(), stats.opts.counterfactual());

  // In both enabled and counterfactual mode, the allocated objects should be
  // correctly accounted for in the top-level allocator.
  BackingStats current_stats = GetAllocatorStats();
  int64_t used = kAllocations * kAllocationSize * kPageSize;
  EXPECT_EQ(GetUsedBytes(current_stats), GetUsedBytes(initial_stats) + used);

  if (GetParam().mode() == LifetimePredictionOptions::Mode::kEnabled) {
    // In enabled mode, we should see the allocations in the lifetime region.
    std::optional<BackingStats> stats = lifetime_allocator_->GetRegionStats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(GetUsedBytes(stats.value()), used);
  } else {
    // In counterfactual mode, we should not see the region allocations in the
    // main allocator's memory usage.
    std::optional<BackingStats> stats = lifetime_allocator_->GetRegionStats();
    ASSERT_FALSE(stats.has_value());
  }

  for (int i = 0; i < kAllocations; ++i) {
    Delete(short_lived_allocs[i]);
  }
}

// Tests the case where we underestimated lifetimes.
TEST_P(LifetimesTest, Expiration) {
  const int kAllocations = 100;
  const int kIterations = 10;

  Span* short_lived_allocs[kAllocations];

  for (int i = 0; i < kAllocations; ++i) {
    short_lived_allocs[i] = AllocateA();  // short-lived
  }

  PRAGMA_NO_UNROLL
  for (int i = 0; i < kIterations; ++i) {
    PRAGMA_NO_UNROLL
    for (int j = 0; j < kAllocations; ++j) {
      Delete(short_lived_allocs[j]);
    }

    PRAGMA_NO_UNROLL
    for (int j = 0; j < kAllocations; ++j) {
      short_lived_allocs[j] = AllocateA();  // short-lived
    }
  }

  // By setting wait1 to a long time period, we can force expiration.
  absl::SleepFor(kWaitLong);

  for (int i = 0; i < kAllocations; i++) {
    Delete(short_lived_allocs[i]);
  }

  LifetimeBasedAllocator::Stats stats = lifetime_allocator_->GetStats();

  EXPECT_EQ(2, stats.database_size);
  EXPECT_EQ(0, stats.database_evictions);

  if (GetParam().always_predict_short_lived()) {
    EXPECT_EQ(1100, stats.tracker.short_lived_predictions);
    EXPECT_EQ(0, stats.tracker.long_lived_predictions);
    EXPECT_EQ(100, stats.tracker.expired_lifetimes);
    EXPECT_EQ(0, stats.tracker.overestimated_lifetimes);
  } else {
    EXPECT_EQ(900, stats.tracker.short_lived_predictions);
    EXPECT_EQ(200, stats.tracker.long_lived_predictions);
    EXPECT_EQ(100, stats.tracker.expired_lifetimes);
    EXPECT_EQ(200, stats.tracker.overestimated_lifetimes);
  }
}

// Tests the case of many parallel threads.
TEST_P(LifetimesTest, ManyThreads) {
  const int kThreads = 10;
  const int kIterations = 100;

  for (int i = 0; i < kIterations; ++i) {
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    absl::Barrier barrier(kThreads);
    for (int j = 0; j < kThreads; ++j) {
      threads.push_back(std::thread([this, &barrier]() {
        Span* alloc = AllocateA();  // short-lived
        barrier.Block();
        Delete(alloc);
      }));
    }

    for (std::thread& thread : threads) {
      thread.join();
    }
  }

  LifetimeBasedAllocator::Stats stats = lifetime_allocator_->GetStats();

  EXPECT_EQ(1, stats.database_size);
  EXPECT_EQ(0, stats.database_evictions);

  // Due to the large number of threads, AddressSanitizer slows down this test
  // significantly, which makes predictions inaccurate. We therefore do not
  // check the precise numbers and only check them in aggregation.
#if !defined(ADDRESS_SANITIZER)
  if (GetParam().always_predict_short_lived()) {
    EXPECT_EQ(kThreads * kIterations, stats.tracker.short_lived_predictions);
    EXPECT_EQ(0, stats.tracker.long_lived_predictions);
    EXPECT_EQ(0, stats.tracker.expired_lifetimes);
    EXPECT_EQ(0, stats.tracker.overestimated_lifetimes);
  } else {
    // The first batch of allocations does not have sufficient profiling data to
    // be placed in the short-lived region, but everything after is.
    EXPECT_EQ(kIterations * kThreads - 20,
              stats.tracker.short_lived_predictions);
    EXPECT_EQ(20, stats.tracker.long_lived_predictions);
    EXPECT_EQ(0, stats.tracker.expired_lifetimes);
    EXPECT_EQ(20, stats.tracker.overestimated_lifetimes);
  }
#else
  EXPECT_EQ(kThreads * kIterations, stats.tracker.short_lived_predictions +
                                        stats.tracker.long_lived_predictions);
#endif
}

// A very thorough, long-running stress test to find corner cases.
TEST_P(LifetimesTest, MultithreadingStressTest) {
  const int kThreads = 512;
  const int kAllocations = 2;
  const int kIterations = 4;
  const int64_t kMagicNumber = 0xabcd123400000000;

  if (!absl::GetFlag(FLAGS_run_all_tests)) {
    // Run with --run_all_tests to enable this test.
    return;
  }

  for (int i = 0; i < kIterations; ++i) {
    std::vector<std::thread> threads;
    std::vector<Span*> allocs;
    threads.reserve(kThreads);
    allocs.resize(kThreads * kAllocations);

    // Allocate from more stack traces than there are slots available to check
    // for race conditions in the stack trace management code.
    for (int j = 0; j < kThreads; ++j) {
      threads.push_back(std::thread([this, i, j, &allocs]() {
        // Allow the thread to get descheduled to make it more likely that the
        // threads will overlap with one another.
        usleep(kThreads - j);

        // Ensure that subsequent iterations have no stack traces in common.
        for (int k = 0; k < kAllocations; ++k) {
          allocs[j + k * kThreads] =
              AllocateWithStacktraceId(j + (i % 2) * kThreads);
          // Write a magic number to each allocated page to detect when a page
          // was allocated multiple times.
          *(static_cast<int64_t*>(allocs[j + k * kThreads]->start_address())) =
              kMagicNumber | (j + k * kThreads);
        }
      }));
    }

    for (std::thread& thread : threads) {
      thread.join();
    }

    // Wait to ensure that everything is long-lived.
    absl::SleepFor(absl::Milliseconds(100));

    // Detect if some memory was corrupted.
    for (int j = 0; j < kThreads * kAllocations; ++j) {
      EXPECT_EQ(*(static_cast<int64_t*>(allocs[j]->start_address())),
                kMagicNumber | j);
      Delete(allocs[j]);
    }
  }

  LifetimeBasedAllocator::Stats stats = lifetime_allocator_->GetStats();

  EXPECT_EQ(1024, stats.database_size);

  // Due to the large number of threads, AddressSanitizer slows down this test
  // significantly, which makes predictions inaccurate. We therefore do not
  // check the precise numbers and only check them in aggregation.
#if !defined(ADDRESS_SANITIZER)
  if (GetParam().always_predict_short_lived()) {
    EXPECT_EQ(kThreads * kAllocations * kIterations,
              stats.tracker.short_lived_predictions);
    EXPECT_EQ(0, stats.tracker.long_lived_predictions);
  } else {
    EXPECT_EQ(0, stats.tracker.short_lived_predictions);
    EXPECT_EQ(kThreads * kAllocations * kIterations,
              stats.tracker.long_lived_predictions);
  }
#else
  EXPECT_EQ(kThreads * kAllocations * kIterations,
            stats.tracker.short_lived_predictions +
                stats.tracker.long_lived_predictions);
#endif
}

// Tests the case where the size of the lifetime database is overflowing.
TEST_P(LifetimesTest, DatabaseEviction) {
  // Test for straightforward eviction.
  Span* first_object = AllocateWithStacktraceId(0);
  for (int i = 1; i < 2100; i++) {
    // Test that we can allocate from an allocation site that has been evicted
    // before without it causing any errors.
    Delete(AllocateWithStacktraceId(i % 2000));
  }

  LifetimeBasedAllocator::Stats stats = lifetime_allocator_->GetStats();
  EXPECT_EQ(1024, stats.database_size);
  EXPECT_EQ(1076, stats.database_evictions);

  // Delete an object whose allocation site has been evicted. This should have
  // no effect, so the best we can test for is the absence of failure.
  Delete(first_object);
}

// Tests for some complicated corner cases in the lifetime-based allocator.
TEST_P(LifetimesTest, CounterfactualDonatedPagesCornerCase) {
  // Produce a donated hugepage as part of a counterfactual allocation, put an
  // object into the filler of that page, then remove the original allocation
  // and then this object. If we do not properly reset the counterfactual
  // pointer after the first deallocation, the allocator will think that the
  // second deallocation is for the donated object and will try to delete the
  // couterfactual object twice.
  for (int i = 0; i < 10; ++i) {
    Span* donated_object =
        allocator_->New(Length(kPagesPerHugePage) - Length(8), 1);
    Span* additional_object = allocator_->New(Length(2), 1);
    Delete(donated_object);
    Delete(additional_object);
  }

  // The same operations in the opposite order.
  for (int i = 0; i < 10; ++i) {
    Span* donated_object =
        allocator_->New(Length(kPagesPerHugePage) - Length(8), 1);
    Span* additional_object = allocator_->New(Length(2), 1);
    Delete(additional_object);
    Delete(donated_object);
  }
}

INSTANTIATE_TEST_SUITE_P(
    LifetimeTests, LifetimesTest, testing::ValuesIn(kTestConfigurations),
    [](const testing::TestParamInfo<LifetimePredictionOptions>& info) {
      return GetTestName(info.param);
    });

// Test enabling lifetime-based allocation at runtime.
class LifetimeBasedAllocatorEnableAtRuntimeTest : public LifetimesTest {
 public:
  explicit LifetimeBasedAllocatorEnableAtRuntimeTest() {
    LifetimePredictionOptions options = LifetimePredictionOptions(
        LifetimePredictionOptions::Mode::kDisabled,
        LifetimePredictionOptions::Strategy::kPredictedLifetimeRegions,
        kLifetimeThreshold);

    // HugePageAwareAllocator can't be destroyed cleanly, so we store a pointer
    // to one and construct in place.
    void* p = malloc(sizeof(HugePageAwareAllocator));
    allocator_ = new (p) HugePageAwareAllocator(
        MemoryTag::kNormal, HugeRegionCountOption::kSlack, options);
    lifetime_allocator_ = &allocator_->lifetime_based_allocator();

    // Warm up the allocator so that objects aren't placed in (regular) regions.
    for (int i = 0; i < 10000; i++) {
      allocator_->New(kPagesPerHugePage / 2 - Length(1), 1);
    }
  }
};

TEST_P(LifetimeBasedAllocatorEnableAtRuntimeTest, Basic) {
  Span* long_lived_allocs[15];
  for (int i = 0; i < 15; ++i) {
    Span* short_lived_allocs[10];
    Span* shorter_lived_allocs[10];

    PRAGMA_NO_UNROLL
    for (int j = 0; j < 10; ++j) {
      short_lived_allocs[j] = AllocateA();  // medium-lived
    }

    long_lived_allocs[i] = AllocateB();  // long-lived

    absl::SleepFor(kWaitLong);

    PRAGMA_NO_UNROLL
    for (int j = 0; j < 10; ++j) {
      shorter_lived_allocs[j] = AllocateA();  // short-lived
    }

    absl::SleepFor(kWaitShort);

    if (i == 4) {
      EXPECT_TRUE(lifetime_allocator_->Enable(GetParam()));
    }

    for (int j = 0; j < 10; ++j) {
      Delete(short_lived_allocs[j]);
      Delete(shorter_lived_allocs[j]);
    }
  }

  for (int i = 0; i < 15; ++i) {
    Delete(long_lived_allocs[i]);
  }

  LifetimeBasedAllocator::Stats stats = lifetime_allocator_->GetStats();

  EXPECT_EQ(3, stats.database_size);
  EXPECT_EQ(0, stats.database_evictions);

  if (GetParam().always_predict_short_lived()) {
    EXPECT_EQ(210, stats.tracker.short_lived_predictions);
    EXPECT_EQ(0, stats.tracker.long_lived_predictions);
    EXPECT_EQ(110, stats.tracker.expired_lifetimes);
    EXPECT_EQ(0, stats.tracker.overestimated_lifetimes);
    EXPECT_EQ(210, stats.region.allocations);
    EXPECT_EQ(53340, stats.region.allocated_pages);
    EXPECT_EQ(210, stats.region.deallocations);
    EXPECT_EQ(53340, stats.region.deallocated_pages);
  } else {
    // The first 20 short-lived allocations are not allocated in the short-lived
    // region because the lifetime-based allocator does not yet have enough
    // profiling data to place them there. Subsequent iterations are placed in
    // the short-lived region.
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
  EXPECT_EQ(kLifetimeThreshold, stats.opts.threshold());
  EXPECT_EQ(GetParam().counterfactual(), stats.opts.counterfactual());
}

INSTANTIATE_TEST_SUITE_P(
    LifetimeTests, LifetimeBasedAllocatorEnableAtRuntimeTest,
    testing::ValuesIn(kTestConfigurations),
    [](const testing::TestParamInfo<LifetimePredictionOptions>& info) {
      return GetTestName(info.param);
    });

TEST(LifetimeOptionsTest, FromFlag) {
  LifetimePredictionOptions opts1 =
      LifetimePredictionOptions::FromFlag("disabled");
  EXPECT_EQ(LifetimePredictionOptions::Mode::kDisabled, opts1.mode());
  EXPECT_FALSE(opts1.error());

  LifetimePredictionOptions opts2 =
      LifetimePredictionOptions::FromFlag("enabled;predict_region;123");
  EXPECT_EQ(LifetimePredictionOptions::Mode::kEnabled, opts2.mode());
  EXPECT_EQ(absl::Milliseconds(123), opts2.threshold());
  EXPECT_EQ(LifetimePredictionOptions::Strategy::kPredictedLifetimeRegions,
            opts2.strategy());
  EXPECT_FALSE(opts2.error());

  LifetimePredictionOptions opts3 =
      LifetimePredictionOptions::FromFlag("counterfactual;always_region;200");
  EXPECT_EQ(LifetimePredictionOptions::Mode::kCounterfactual, opts3.mode());
  EXPECT_EQ(absl::Milliseconds(200), opts3.threshold());
  EXPECT_EQ(LifetimePredictionOptions::Strategy::kAlwaysShortLivedRegions,
            opts3.strategy());
  EXPECT_FALSE(opts3.error());

  LifetimePredictionOptions opts4 =
      LifetimePredictionOptions::FromFlag("counterfactual;invalid;200");
  EXPECT_TRUE(opts4.error());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
