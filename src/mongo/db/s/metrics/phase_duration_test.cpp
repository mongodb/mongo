/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/s/metrics/phase_duration.h"

#include "mongo/base/string_data.h"
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
