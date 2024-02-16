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

#include "tcmalloc/internal/timeseries_tracker.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::ElementsAre;

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class TimeSeriesTrackerTest : public testing::Test {
 public:
  struct TestEntry {
    static TestEntry Nil() { return TestEntry(); }

    void Report(int n) { values_.push_back(n); }

    bool empty() const { return values_.empty(); }

    std::vector<int> values_;
  };

 protected:
  void Advance(absl::Duration d) {
    clock_ += absl::ToDoubleSeconds(d) * GetFakeClockFrequency();
  }

  static constexpr absl::Duration kDuration = absl::Seconds(2);

  TimeSeriesTracker<TestEntry, int, 8> tracker_{
      Clock{.now = FakeClock, .freq = GetFakeClockFrequency}, kDuration};

 private:
  static int64_t FakeClock() { return clock_; }

  static double GetFakeClockFrequency() {
    return absl::ToDoubleNanoseconds(absl::Seconds(2));
  }

  static int64_t clock_;
};

int64_t TimeSeriesTrackerTest::clock_{0};

// Test that frequency conversion in the cycle clock works correctly
TEST(TimeSeriesTest, CycleClock) {
  TimeSeriesTracker<TimeSeriesTrackerTest::TestEntry, int, 100> tracker{
      Clock{absl::base_internal::CycleClock::Now,
            absl::base_internal::CycleClock::Frequency},
      absl::Seconds(10)};  // 100ms epochs

  tracker.Report(1);
  absl::SleepFor(absl::Milliseconds(100));
  tracker.Report(2);

  // Iterate through entries skipping empty entries.
  int num_timestamps = 0;
  int offset_1, offset_2;
  tracker.Iter(
      [&](size_t offset, int64_t ts,
          const TimeSeriesTrackerTest::TestEntry& e) {
        ASSERT_LT(num_timestamps, 2);
        if (num_timestamps == 0) {
          offset_1 = offset;
          EXPECT_THAT(e.values_, ElementsAre(1));
        } else {
          offset_2 = offset;
          EXPECT_THAT(e.values_, ElementsAre(2));
        }
        num_timestamps++;
      },
      tracker.kSkipEmptyEntries);

  // If we are near an epoch boundary, we may skip two epochs.
  EXPECT_GE(offset_2 - offset_1, 1);
  EXPECT_LE(offset_2 - offset_1, 2);
}

TEST_F(TimeSeriesTrackerTest, Works) {
  const int64_t kEpochLength = absl::ToInt64Nanoseconds(kDuration) / 8;
  Advance(kDuration);

  tracker_.Report(1);
  Advance(absl::Nanoseconds(1));
  tracker_.Report(2);
  Advance(kDuration / 4);
  tracker_.Report(4);

  // Iterate through entries skipping empty entries.
  int num_timestamps = 0;
  int offset_1, offset_2;
  tracker_.Iter(
      [&](size_t offset, int64_t ts, const TestEntry& e) {
        ASSERT_LT(num_timestamps, 2);
        if (num_timestamps == 0) {
          offset_1 = offset;
          EXPECT_EQ(absl::ToInt64Nanoseconds(kDuration), ts);
          EXPECT_THAT(e.values_, ElementsAre(1, 2));
        } else {
          offset_2 = offset;
          EXPECT_EQ(absl::ToInt64Nanoseconds(kDuration) +
                        absl::ToInt64Nanoseconds(kDuration) / 4,
                    ts);
          EXPECT_THAT(e.values_, ElementsAre(4));
        }
        num_timestamps++;
      },
      tracker_.kSkipEmptyEntries);

  EXPECT_EQ(2, num_timestamps);
  EXPECT_EQ(offset_2 - offset_1, 2);

  Advance(kDuration / 4);

  // Iterate through entries not skipping empty entries.
  int64_t expected_timestamp = absl::ToInt64Nanoseconds(kDuration) / 4;
  num_timestamps = 0;

  tracker_.Iter(
      [&](size_t offset, int64_t ts, const TestEntry& e) {
        expected_timestamp += kEpochLength;
        ASSERT_LT(num_timestamps, 8);
        EXPECT_EQ(expected_timestamp, ts);
        num_timestamps++;
      },
      tracker_.kDoNotSkipEmptyEntries);

  EXPECT_EQ(8, num_timestamps);

  tracker_.Report(8);
  Advance(kDuration / 4);
  tracker_.Report(16);

  // Iterate backwards.
  num_timestamps = 0;
  expected_timestamp =
      7 * absl::ToInt64Nanoseconds(kDuration) / 4;  // Current time
  tracker_.IterBackwards(
      [&](size_t offset, int64_t ts, const TestEntry& e) {
        ASSERT_LT(num_timestamps, 3);
        EXPECT_EQ(num_timestamps, offset);
        EXPECT_EQ(expected_timestamp, ts);
        if (num_timestamps == 0) {
          EXPECT_THAT(e.values_, ElementsAre(16));
        } else if (num_timestamps == 1) {
          EXPECT_TRUE(e.values_.empty());
        } else {
          EXPECT_THAT(e.values_, ElementsAre(8));
        }
        expected_timestamp -= kEpochLength;
        num_timestamps++;
      },
      3);

  EXPECT_EQ(3, num_timestamps);

  EXPECT_THAT(tracker_.GetEpochAtOffset(0).values_, ElementsAre(16));
  EXPECT_THAT(tracker_.GetEpochAtOffset(2).values_, ElementsAre(8));
  EXPECT_TRUE(tracker_.GetEpochAtOffset(3).empty());
  EXPECT_TRUE(tracker_.GetEpochAtOffset(1000).empty());

  // This should annilate everything.
  Advance(kDuration * 2);
  tracker_.UpdateTimeBase();
  tracker_.Iter(
      [&](size_t offset, int64_t ts, const TestEntry& e) {
        ASSERT_TRUE(false) << "Time series should be empty";
      },
      tracker_.kSkipEmptyEntries);

  EXPECT_TRUE(tracker_.GetEpochAtOffset(1).empty());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
