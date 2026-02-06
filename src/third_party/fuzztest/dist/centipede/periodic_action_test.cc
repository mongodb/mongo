// Copyright 2024 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/periodic_action.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <thread>  // NOLINT: For `std::this_thread::get_id()` only.
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./common/logging.h"

namespace fuzztest::internal {
namespace {

TEST(PeriodicActionTest, OnlyPeriodicInvocations) {
  constexpr absl::Duration kDuration = absl::Seconds(3);
  constexpr absl::Duration kPeriodicInterval = absl::Milliseconds(100);
  const int kApproxCount =
      std::floor(absl::FDivDuration(kDuration, kPeriodicInterval));
  int count = 0;
  PeriodicAction action{
      [&count]() { ++count; },
      PeriodicAction::ZeroDelayConstInterval(kPeriodicInterval),
  };
  absl::SleepFor(kDuration);
  action.Stop();
  EXPECT_GE(count, kApproxCount * 0.9) << VV(kApproxCount);
  EXPECT_LE(count, kApproxCount * 1.1) << VV(kApproxCount);
}

TEST(PeriodicActionTest, OnlyNudgedInvocations) {
  constexpr absl::Duration kDuration = absl::Seconds(3);
  constexpr absl::Duration kNudgeInterval = absl::Milliseconds(100);
  int count = 0;
  PeriodicAction::Options options;
  // Effectively disable periodic invocations: only `Nudge()` calls
  // below will trigger them.
  options.sleep_before_each = [](size_t) { return absl::InfiniteDuration(); };
  PeriodicAction action{
      [&count]() { ++count; },
      std::move(options),
  };
  int expected_count = 0;
  const absl::Time end_time = absl::Now() + kDuration;
  while (absl::Now() < end_time) {
    action.Nudge();
    // Sleep after a nudge, not before, to guarantee that the action has time
    // to finish and increment `count`.
    absl::SleepFor(kNudgeInterval);
    ++expected_count;
  }
  action.Stop();
  EXPECT_GE(count, expected_count * 0.9) << VV(expected_count);
  EXPECT_LE(count, expected_count * 1.1) << VV(expected_count);
}

TEST(PeriodicActionTest, PeriodicAndNudgedInvocations) {
  constexpr absl::Duration kDuration = absl::Seconds(3);
  constexpr absl::Duration kPeriodicInterval = absl::Milliseconds(100);
  // NOTE: Use a nudge interval that is not wholly divisible by the periodic
  // interval so the two events never clash. This is to make `count`
  // incrementing more deterministic so that tighter bounds on its final value
  // can be asserted. A looser version with clashing periodic and nudged
  // invocations is implemented in another test case below.
  constexpr absl::Duration kNudgeInterval = absl::Milliseconds(345);
  const int kApproxPeriodicCount =
      std::floor(absl::FDivDuration(kDuration, kPeriodicInterval));
  const int kApproxNudgedCount =
      std::floor(absl::FDivDuration(kDuration, kNudgeInterval));
  const int kApproxCount = kApproxPeriodicCount + kApproxNudgedCount;
  int count = 0;
  PeriodicAction action{
      [&count]() { ++count; },
      PeriodicAction::ZeroDelayConstInterval(kPeriodicInterval),
  };
  const absl::Time end_time = absl::Now() + kDuration;
  while (absl::Now() < end_time) {
    action.Nudge();
    // Sleep after a nudge, not before, to guarantee that the action has time to
    // finish and increment `count`.
    absl::SleepFor(kNudgeInterval);
  }
  action.Stop();
  EXPECT_GE(count, kApproxCount * 0.9)
      << VV(kApproxCount) << VV(kApproxPeriodicCount) << VV(kApproxNudgedCount);
  EXPECT_LE(count, kApproxCount * 1.1)
      << VV(kApproxCount) << VV(kApproxPeriodicCount) << VV(kApproxNudgedCount);
}

TEST(PeriodicActionTest, ClashingPeriodicAndNudgedInvocations) {
  constexpr absl::Duration kDuration = absl::Seconds(3);
  constexpr absl::Duration kPeriodicInterval = absl::Milliseconds(10);
  // NOTE: Use a nudge interval that is wholly divisible by the periodic
  // interval so the two events overlap with high probability.
  constexpr absl::Duration kNudgeInterval = absl::Milliseconds(2);
  const int kMaxPeriodicCount =
      std::floor(absl::FDivDuration(kDuration, kPeriodicInterval));
  const int kMaxNudgedCount =
      std::floor(absl::FDivDuration(kDuration, kNudgeInterval));
  int count = 0;
  PeriodicAction action{
      [&count]() { ++count; },
      PeriodicAction::ZeroDelayConstInterval(kPeriodicInterval),
  };
  const absl::Time end_time = absl::Now() + kDuration;
  while (absl::Now() < end_time) {
    action.Nudge();
    // Sleep after a nudge, not before, to guarantee that the action has time to
    // finish and increment `count`.
    absl::SleepFor(kNudgeInterval);
  }
  action.Stop();
  // The frequent nudging should have interrupted the sleeping phase and reset
  // the periodic timer a lot, so we can assert only very loose bounds on the
  // final value of `count`.
  EXPECT_GE(count, std::min(kMaxPeriodicCount, kMaxNudgedCount))
      << VV(kMaxPeriodicCount) << VV(kMaxNudgedCount);
  EXPECT_LE(count, kMaxPeriodicCount + kMaxNudgedCount)
      << VV(kMaxPeriodicCount) << VV(kMaxNudgedCount);
}

// Test that a `Nudge()` immediately followed by an explicit `Stop()` still
// runs the action.
TEST(PeriodicActionTest, NudgeThenStopStillRunsAction) {
  int count = 0;
  absl::Mutex count_mu;
  PeriodicAction action{
      [&count, &count_mu]() {
        absl::MutexLock lock{&count_mu};
        ++count;
      },
      PeriodicAction::ZeroDelayConstInterval(absl::InfiniteDuration()),
  };
  absl::SleepFor(absl::Seconds(1));
  {
    absl::MutexLock lock{&count_mu};
    EXPECT_EQ(count, 1);
  }
  action.Nudge();
  action.Stop();
  {
    absl::MutexLock lock{&count_mu};
    EXPECT_EQ(count, 2);
  }
}

// Test that a `Nudge()` immediately followed by an implicit `Stop()` in
// `~PeriodicAction()` still runs the action.
TEST(PeriodicActionTest, NudgeThenDtorStillRunsAction) {
  int count = 0;
  absl::Mutex count_mu;
  {
    PeriodicAction action{
        [&count, &count_mu]() {
          absl::MutexLock lock{&count_mu};
          ++count;
        },
        PeriodicAction::ZeroDelayConstInterval(absl::InfiniteDuration()),
    };
    absl::SleepFor(absl::Seconds(1));
    {
      absl::MutexLock lock{&count_mu};
      EXPECT_EQ(count, 1);
    }
    EXPECT_EQ(count, 1);
    action.Nudge();
  }
  {
    absl::MutexLock lock{&count_mu};
    EXPECT_EQ(count, 2);
  }
}

// The main purpose of this test is to make sure that a `PeriodicAction` object
// can be moved to another such that the original object's dtor doesn't blow up
// when it runs.
TEST(PeriodicActionTest, ActionIsMoveable) {
  absl::Mutex mu;
  std::vector<std::thread::id> thread_ids;
  {
    PeriodicAction moved_from{
        [&mu, &thread_ids]() {
          absl::WriterMutexLock lock{&mu};
          thread_ids.push_back(std::this_thread::get_id());
        },
        PeriodicAction::ZeroDelayConstInterval(absl::Milliseconds(10)),
    };
    absl::SleepFor(absl::Milliseconds(100));
    // Sanity check that the action is running and is healthy.
    moved_from.Nudge();
    absl::SleepFor(absl::Milliseconds(100));
    // Move the action to another object.
    PeriodicAction moved_to = std::move(moved_from);
    absl::SleepFor(absl::Milliseconds(100));
    // The moved object should now be running the run-loop thread.
    moved_to.Nudge();
    absl::SleepFor(absl::Milliseconds(100));
    moved_to.Stop();
  }  // The dtors for both moved-from and moved-to objects run here.
  // If we reached this point, at least the dtors ran without blowing up.
  ASSERT_GT(thread_ids.size(), 1);
  // A single instance of the run-loop thread should have been running
  // throughout the whole process, including the move: the moved-from object
  // should have just handed over the thread to the moved-to object.
  std::sort(thread_ids.begin(), thread_ids.end());
  ASSERT_EQ(thread_ids.front(), thread_ids.back());
}

}  // namespace
}  // namespace fuzztest::internal
