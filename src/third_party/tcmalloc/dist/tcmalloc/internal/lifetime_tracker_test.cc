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

#include "tcmalloc/internal/lifetime_tracker.h"

#include <stdint.h>

#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/lifetime_predictions.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class MockLifetimeStats {
 public:
  enum class Prediction { kShortLived, kLongLived };
  MOCK_METHOD(void, Update, (Prediction prediction), ());
};

class MockLifetimeDatabase {
 public:
  MOCK_METHOD(void, RemoveLifetimeStatsReference, (MockLifetimeStats*), ());
};

using LifetimeTrackerUnderTest =
    LifetimeTrackerImpl<MockLifetimeDatabase, MockLifetimeStats>;

class LifetimeTrackerTest : public testing::Test {
 protected:
  const Clock kFakeClock =
      Clock{.now = FakeClock, .freq = GetFakeClockFrequency};

  void Advance(absl::Duration d) {
    clock_ += absl::ToDoubleSeconds(d) * GetFakeClockFrequency();
  }

 private:
  static int64_t FakeClock() { return clock_; }

  static double GetFakeClockFrequency() {
    return absl::ToDoubleNanoseconds(absl::Seconds(2));
  }

  static int64_t clock_;
};

int64_t LifetimeTrackerTest::clock_{0};

TEST_F(LifetimeTrackerTest, Basic) {
  MockLifetimeDatabase database;
  LifetimeTrackerUnderTest tracker(&database, absl::Seconds(0.5), kFakeClock);
  MockLifetimeStats stats;

  LifetimeTrackerUnderTest::Tracker tracker1;
  tracker.AddAllocation(&tracker1, &stats, false);
  Advance(absl::Seconds(1));

  EXPECT_CALL(stats, Update(MockLifetimeStats::Prediction::kLongLived));
  EXPECT_CALL(database, RemoveLifetimeStatsReference(&stats));

  LifetimeTrackerUnderTest::Tracker tracker2;
  tracker.AddAllocation(&tracker2, &stats, false);

  EXPECT_CALL(stats, Update(MockLifetimeStats::Prediction::kShortLived));
  EXPECT_CALL(database, RemoveLifetimeStatsReference(&stats));

  Advance(absl::Seconds(0.1));
  tracker.RemoveAllocation(&tracker2);

  EXPECT_EQ(tracker.stats().expired_lifetimes, 0);
  EXPECT_EQ(tracker.stats().overestimated_lifetimes, 1);
  EXPECT_EQ(tracker.stats().short_lived_predictions, 0);
  EXPECT_EQ(tracker.stats().long_lived_predictions, 2);
}

TEST_F(LifetimeTrackerTest, ExpirationLogic) {
  MockLifetimeDatabase database;
  LifetimeTrackerUnderTest tracker(&database, absl::Seconds(0.5), kFakeClock);

  // Create 100 trackers, all predicted short-lived. Every second tracker will
  // be long-lived and therefore expire.
  const int kNumTrackers = 100;
  std::vector<LifetimeTrackerUnderTest::Tracker> trackers(kNumTrackers);
  MockLifetimeStats stats[] = {MockLifetimeStats(), MockLifetimeStats()};

  for (int i = 0; i < kNumTrackers; ++i) {
    tracker.AddAllocation(&trackers[i], &stats[i % 2], true);
    Advance(absl::Milliseconds(1));
  }

  EXPECT_CALL(stats[0], Update(MockLifetimeStats::Prediction::kShortLived))
      .Times(kNumTrackers / 2);
  EXPECT_CALL(database, RemoveLifetimeStatsReference(&stats[0]))
      .Times(kNumTrackers / 2);

  for (int i = 0; i < kNumTrackers; i += 2) {
    tracker.RemoveAllocation(&trackers[i]);
  }

  // After an additional 450ms, 1/4 of the allocations should have expired.
  EXPECT_CALL(stats[1], Update(MockLifetimeStats::Prediction::kLongLived))
      .Times(kNumTrackers / 4);
  EXPECT_CALL(database, RemoveLifetimeStatsReference(&stats[1]))
      .Times(kNumTrackers / 4);

  Advance(absl::Milliseconds(450));
  tracker.CheckForLifetimeExpirations();

  EXPECT_EQ(tracker.stats().expired_lifetimes, kNumTrackers / 4);
  EXPECT_EQ(tracker.stats().overestimated_lifetimes, 0);
  EXPECT_EQ(tracker.stats().short_lived_predictions, kNumTrackers);
  EXPECT_EQ(tracker.stats().long_lived_predictions, 0);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
