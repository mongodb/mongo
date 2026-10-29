// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/metrics/phase_duration.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

constexpr auto kDelta = Milliseconds{1234};

ClockSourceMock clock;

TEST(ShardingMetricsPhaseDurationTest, PhaseDurationStartsWithNoStartTime) {
    PhaseDuration d;
    ASSERT_EQ(d.getStart(), boost::none);
}

TEST(ShardingMetricsPhaseDurationTest, PhaseDurationStartsWithNoEndTime) {
    PhaseDuration d;
    ASSERT_EQ(d.getEnd(), boost::none);
}

TEST(ShardingMetricsPhaseDurationTest, PhaseDurationSetStartTime) {
    PhaseDuration d;
    const auto time = clock.now();
    d.setStart(time);
    ASSERT_EQ(d.getStart(), time);
}

TEST(ShardingMetricsPhaseDurationTest, PhaseDurationSetEndTime) {
    PhaseDuration d;
    const auto time = clock.now();
    d.setEnd(time);
    ASSERT_EQ(d.getEnd(), time);
}

TEST(ShardingMetricsPhaseDurationTest, GetElapsedReturnsNoneIfNotStarted) {
    PhaseDuration d;
    ASSERT_EQ(d.getElapsed<Milliseconds>(&clock), boost::none);
}

TEST(ShardingMetricsPhaseDurationTest, GetElapsedReturnsElapsedTime) {
    PhaseDuration d;
    d.setStart(clock.now());
    clock.advance(kDelta);
    d.setEnd(clock.now());
    ASSERT_EQ(d.getElapsed<Milliseconds>(&clock), kDelta);
}

TEST(ShardingMetricsPhaseDurationTest, GetElapsedReturnsElapsedTimeAsRoundedSeconds) {
    PhaseDuration d;
    clock.reset(Date_t::fromMillisSinceEpoch(0));
    d.setStart(clock.now());
    clock.advance(kDelta);
    d.setEnd(clock.now());
    ASSERT_EQ(d.getElapsed<Seconds>(&clock), Seconds{1});
}

TEST(ShardingMetricsPhaseDurationTest, GetElapsedReturnsCurrentElapsedIfNotDone) {
    PhaseDuration d;
    d.setStart(clock.now());
    clock.advance(kDelta);
    ASSERT_EQ(d.getElapsed<Milliseconds>(&clock), kDelta);
}

TEST(ShardingMetricsPhaseDurationTest, GetElapsedReturnsElapsedAfterDoneAndMoreTimePasses) {
    PhaseDuration d;
    d.setStart(clock.now());
    clock.advance(kDelta);
    d.setEnd(clock.now());
    clock.advance(kDelta);
    ASSERT_EQ(d.getElapsed<Milliseconds>(&clock), kDelta);
}

}  // namespace
}  // namespace mongo
