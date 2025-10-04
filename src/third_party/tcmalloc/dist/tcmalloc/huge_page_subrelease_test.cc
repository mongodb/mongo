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

#include "tcmalloc/huge_page_subrelease.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <tuple>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/macros.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/memory/memory.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/blocking_counter.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

using tcmalloc::tcmalloc_internal::Length;
using testing::StrEq;

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class StatsTrackerTest : public testing::Test {
 private:
  static int64_t clock_;
  static int64_t FakeClock() { return clock_; }
  static double GetFakeClockFrequency() {
    return absl::ToDoubleNanoseconds(absl::Seconds(2));
  }

 protected:
  static constexpr absl::Duration kWindow = absl::Minutes(10);

  using StatsTrackerType = SubreleaseStatsTracker<16>;
  StatsTrackerType tracker_{
      Clock{.now = FakeClock, .freq = GetFakeClockFrequency}, kWindow,
      absl::Minutes(5)};

  void Advance(absl::Duration d) {
    clock_ += static_cast<int64_t>(absl::ToDoubleSeconds(d) *
                                   GetFakeClockFrequency());
  }

  // Generates four data points for the tracker that represent "interesting"
  // points (i.e., min/max pages demand, min/max hugepages).
  void GenerateInterestingPoints(Length num_pages, HugeLength num_hugepages,
                                 Length num_free_pages);

  // Generates a data point with a particular amount of demand pages, while
  // ignoring the specific number of hugepages.
  void GenerateDemandPoint(Length num_pages, Length num_free_pages);

  void SetUp() override {
    // Resets the clock used by SubreleaseStatsTracker, allowing each test
    // starts in epoch 0.
    clock_ = 0;
  }
};

int64_t StatsTrackerTest::clock_{0};

void StatsTrackerTest::GenerateInterestingPoints(Length num_pages,
                                                 HugeLength num_hugepages,
                                                 Length num_free_pages) {
  for (int i = 0; i <= 1; ++i) {
    for (int j = 0; j <= 1; ++j) {
      StatsTrackerType::SubreleaseStats stats;
      stats.num_pages = num_pages + Length((i == 0) ? 4 : 8 * j);
      stats.free_pages = num_free_pages + Length(10 * i + j);
      stats.unmapped_pages = Length(10);
      stats.used_pages_in_subreleased_huge_pages = num_pages;
      stats.huge_pages[StatsTrackerType::kRegular] =
          num_hugepages + ((i == 1) ? NHugePages(4) : NHugePages(8) * j);
      stats.huge_pages[StatsTrackerType::kDonated] = num_hugepages;
      stats.huge_pages[StatsTrackerType::kPartialReleased] = NHugePages(i);
      stats.huge_pages[StatsTrackerType::kReleased] = NHugePages(j);
      tracker_.Report(stats);
    }
  }
}

void StatsTrackerTest::GenerateDemandPoint(Length num_pages,
                                           Length num_free_pages) {
  HugeLength hp = NHugePages(1);
  StatsTrackerType::SubreleaseStats stats;
  stats.num_pages = num_pages;
  stats.free_pages = num_free_pages;
  stats.unmapped_pages = Length(0);
  stats.used_pages_in_subreleased_huge_pages = Length(0);
  stats.huge_pages[StatsTrackerType::kRegular] = hp;
  stats.huge_pages[StatsTrackerType::kDonated] = hp;
  stats.huge_pages[StatsTrackerType::kPartialReleased] = hp;
  stats.huge_pages[StatsTrackerType::kReleased] = hp;
  tracker_.Report(stats);
}

// Tests that the tracker aggregates all data correctly. The output is tested by
// comparing the text output of the tracker. While this is a bit verbose, it is
// much cleaner than extracting and comparing all data manually.
TEST_F(StatsTrackerTest, Works) {
  // Ensure that the beginning (when free pages are 0) is outside the 5-min
  // window the instrumentation is recording.
  GenerateInterestingPoints(Length(1), NHugePages(1), Length(1));
  Advance(absl::Minutes(5));

  GenerateInterestingPoints(Length(100), NHugePages(5), Length(200));

  Advance(absl::Minutes(1));

  GenerateInterestingPoints(Length(200), NHugePages(10), Length(100));

  Advance(absl::Minutes(1));

  // Test text output (time series summary).
  {
    std::string buffer(1024 * 1024, '\0');
    Printer printer(&*buffer.begin(), buffer.size());
    {
      tracker_.Print(&printer, "StatsTracker");
      buffer.erase(printer.SpaceRequired());
    }

    EXPECT_THAT(buffer, StrEq(R"(StatsTracker: time series over 5 min interval

StatsTracker: realized fragmentation: 0.8 MiB
StatsTracker: minimum free pages: 110 (100 backed)
StatsTracker: at peak demand: 208 pages (and 111 free, 10 unmapped)
StatsTracker: at peak demand: 26 hps (14 regular, 10 donated, 1 partial, 1 released)
StatsTracker: at peak hps: 208 pages (and 111 free, 10 unmapped)
StatsTracker: at peak hps: 26 hps (14 regular, 10 donated, 1 partial, 1 released)

StatsTracker: Since the start of the execution, 0 subreleases (0 pages) were skipped due to either recent (0s) peaks, or the sum of short-term (0s) fluctuations and long-term (0s) trends.
StatsTracker: 0.0000% of decisions confirmed correct, 0 pending (0.0000% of pages, 0 pending), as per anticipated 0s realized fragmentation.
StatsTracker: Subrelease stats last 10 min: total 0 pages subreleased (0 pages from partial allocs), 0 hugepages broken
)"));
  }
}

TEST_F(StatsTrackerTest, InvalidDurations) {
  // These should not crash.
  tracker_.min_free_pages(absl::InfiniteDuration());
  tracker_.min_free_pages(kWindow + absl::Seconds(1));
  tracker_.min_free_pages(-(kWindow + absl::Seconds(1)));
  tracker_.min_free_pages(-absl::InfiniteDuration());
}

TEST_F(StatsTrackerTest, ComputeRecentPeaks) {
  GenerateDemandPoint(Length(3000), Length(1000));
  Advance(absl::Minutes(1.25));
  GenerateDemandPoint(Length(1500), Length(0));
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(100), Length(2000));
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(500), Length(3000));

  Length peak = tracker_.GetRecentPeak(absl::Minutes(3));
  EXPECT_EQ(peak, Length(1500));
  Length peak2 = tracker_.GetRecentPeak(absl::Minutes(5));
  EXPECT_EQ(peak2, Length(3000));

  Advance(absl::Minutes(4));
  GenerateDemandPoint(Length(200), Length(3000));

  Length peak3 = tracker_.GetRecentPeak(absl::Minutes(4));
  EXPECT_EQ(peak3, Length(200));

  Advance(absl::Minutes(5));
  GenerateDemandPoint(Length(150), Length(3000));

  Length peak4 = tracker_.GetRecentPeak(absl::Minutes(5));
  EXPECT_EQ(peak4, Length(150));
}

TEST_F(StatsTrackerTest, ComputeRecentDemand) {
  // Generates max and min demand in each epoch to create short-term demand
  // fluctuations.
  GenerateDemandPoint(Length(1500), Length(2000));
  GenerateDemandPoint(Length(3000), Length(1000));
  Advance(absl::Minutes(1.25));
  GenerateDemandPoint(Length(500), Length(1000));
  GenerateDemandPoint(Length(1500), Length(0));
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(50), Length(1000));
  GenerateDemandPoint(Length(100), Length(2000));
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(100), Length(2000));
  GenerateDemandPoint(Length(300), Length(3000));

  Length short_long_peak_pages =
      tracker_.GetRecentDemand(absl::Minutes(2), absl::Minutes(3));
  EXPECT_EQ(short_long_peak_pages, Length(700));
  Length short_long_peak_pages2 =
      tracker_.GetRecentDemand(absl::Minutes(5), absl::Minutes(5));
  EXPECT_EQ(short_long_peak_pages2, Length(3000));

  Advance(absl::Minutes(4));
  GenerateDemandPoint(Length(150), Length(500));
  GenerateDemandPoint(Length(200), Length(3000));

  Length short_long_peak_pages3 =
      tracker_.GetRecentDemand(absl::Minutes(1), absl::ZeroDuration());
  EXPECT_EQ(short_long_peak_pages3, Length(50));

  Advance(absl::Minutes(5));
  GenerateDemandPoint(Length(100), Length(700));
  GenerateDemandPoint(Length(150), Length(800));

  Length short_long_peak_pages4 =
      tracker_.GetRecentDemand(absl::ZeroDuration(), absl::Minutes(5));
  EXPECT_EQ(short_long_peak_pages4, Length(100));
  // The short_interval needs to be shorter or equal to the long_interval when
  // they are both set.  We cap short_interval to long_interval when this is not
  // the case.
  EXPECT_EQ(tracker_.GetRecentDemand(absl::Minutes(2), absl::Minutes(1)),
            tracker_.GetRecentDemand(absl::Minutes(1), absl::Minutes(1)));
}

TEST_F(StatsTrackerTest, TrackCorrectSubreleaseDecisions) {
  // First peak (large)
  GenerateDemandPoint(Length(1000), Length(1000));

  // Incorrect subrelease: Subrelease to 1000
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(100), Length(1000));
  tracker_.ReportSkippedSubreleasePages(Length(900), Length(1000));

  // Second peak (small)
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(500), Length(1000));

  EXPECT_EQ(tracker_.total_skipped().pages, Length(900));
  EXPECT_EQ(tracker_.total_skipped().count, 1);
  EXPECT_EQ(tracker_.correctly_skipped().pages, Length(0));
  EXPECT_EQ(tracker_.correctly_skipped().count, 0);
  EXPECT_EQ(tracker_.pending_skipped().pages, Length(900));
  EXPECT_EQ(tracker_.pending_skipped().count, 1);

  // Correct subrelease: Subrelease to 500
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(500), Length(100));
  tracker_.ReportSkippedSubreleasePages(Length(50), Length(550));
  GenerateDemandPoint(Length(500), Length(50));
  tracker_.ReportSkippedSubreleasePages(Length(50), Length(500));
  GenerateDemandPoint(Length(500), Length(0));

  EXPECT_EQ(tracker_.total_skipped().pages, Length(1000));
  EXPECT_EQ(tracker_.total_skipped().count, 3);
  EXPECT_EQ(tracker_.correctly_skipped().pages, Length(0));
  EXPECT_EQ(tracker_.correctly_skipped().count, 0);
  EXPECT_EQ(tracker_.pending_skipped().pages, Length(1000));
  EXPECT_EQ(tracker_.pending_skipped().count, 3);

  // Third peak (large, too late for first peak)
  Advance(absl::Minutes(4));
  GenerateDemandPoint(Length(1100), Length(1000));

  Advance(absl::Minutes(5));
  GenerateDemandPoint(Length(1100), Length(1000));

  EXPECT_EQ(tracker_.total_skipped().pages, Length(1000));
  EXPECT_EQ(tracker_.total_skipped().count, 3);
  EXPECT_EQ(tracker_.correctly_skipped().pages, Length(100));
  EXPECT_EQ(tracker_.correctly_skipped().count, 2);
  EXPECT_EQ(tracker_.pending_skipped().pages, Length(0));
  EXPECT_EQ(tracker_.pending_skipped().count, 0);
}

TEST_F(StatsTrackerTest, SubreleaseCorrectnessWithChangingIntervals) {
  // First peak (large)
  GenerateDemandPoint(Length(1000), Length(1000));

  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(100), Length(1000));

  tracker_.ReportSkippedSubreleasePages(Length(50), Length(1000),
                                        absl::Minutes(4));
  Advance(absl::Minutes(1));

  // With two correctness intervals in the same epoch, take the maximum
  tracker_.ReportSkippedSubreleasePages(Length(100), Length(1000),
                                        absl::Minutes(1));
  tracker_.ReportSkippedSubreleasePages(Length(200), Length(1000),
                                        absl::Minutes(7));

  Advance(absl::Minutes(5));
  GenerateDemandPoint(Length(1100), Length(1000));
  Advance(absl::Minutes(10));
  GenerateDemandPoint(Length(1100), Length(1000));

  EXPECT_EQ(tracker_.total_skipped().pages, Length(350));
  EXPECT_EQ(tracker_.total_skipped().count, 3);
  EXPECT_EQ(tracker_.correctly_skipped().pages, Length(300));
  EXPECT_EQ(tracker_.correctly_skipped().count, 2);
  EXPECT_EQ(tracker_.pending_skipped().pages, Length(0));
  EXPECT_EQ(tracker_.pending_skipped().count, 0);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
