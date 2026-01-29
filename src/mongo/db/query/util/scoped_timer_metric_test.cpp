/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/util/scoped_timer_metric.h"

#include "mongo/db/stats/counters.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {


TEST(ScopedTimerMetricTest, IncrementCounterOnDestruction) {
    TickSourceMock<Milliseconds> tickSource;
    DurationCounter64<Milliseconds> counter;

    // Set the tick source to advance by 100ms each time getTicks() is called.
    // First call (in constructor) returns 0, second call (in destructor) returns 100.
    tickSource.setAdvanceOnRead(Milliseconds{100});

    {
        ScopedTimerMetric timer(&tickSource, counter);
    }

    ASSERT_EQ(counter.get(), 100);
}

TEST(ScopedTimerMetricTest, ZeroDurationWhenNoTimeElapsed) {
    TickSourceMock<Milliseconds> tickSource;
    DurationCounter64<Milliseconds> counter;

    // No advance on read - both getTicks() calls return the same value.
    {
        ScopedTimerMetric timer(&tickSource, counter);
    }

    ASSERT_EQ(counter.get(), 0);
}

TEST(ScopedTimerMetricTest, MultipleTimersAccumulate) {
    TickSourceMock<Milliseconds> tickSource;
    DurationCounter64<Milliseconds> counter;

    tickSource.setAdvanceOnRead(Milliseconds{50});

    {
        ScopedTimerMetric timer1(&tickSource, counter);
    }
    {
        ScopedTimerMetric timer2(&tickSource, counter);
    }
    {
        ScopedTimerMetric timer3(&tickSource, counter);
    }

    // Each timer should add 50ms (the advance on read amount).
    ASSERT_EQ(counter.get(), 150);
}

TEST(ScopedTimerMetricTest, WorksWithMicroseconds) {
    TickSourceMock<Microseconds> tickSource;
    DurationCounter64<Microseconds> counter;

    tickSource.setAdvanceOnRead(Microseconds{500});

    {
        ScopedTimerMetric timer(&tickSource, counter);
    }

    ASSERT_EQ(counter.get(), 500);
}

TEST(ScopedTimerMetricTest, DifferentDurationTypes) {
    // Use a tick source with a different duration base than the timer.
    TickSourceMock<Microseconds> tickSource;
    DurationCounter64<Milliseconds> counter;

    tickSource.setAdvanceOnRead(Microseconds{5000});

    {
        ScopedTimerMetric timer(&tickSource, counter);
    }

    ASSERT_EQ(counter.get(), 5);
}

TEST(ScopedTimerMetricTest, ManualAdvanceSimulatesTimePassage) {
    TickSourceMock<Milliseconds> tickSource;
    DurationCounter64<Milliseconds> counter;

    {
        ScopedTimerMetric timer(&tickSource, counter);
        // Manually advance the tick source to simulate 250ms passing.
        tickSource.advance(Milliseconds{250});
    }

    ASSERT_EQ(counter.get(), 250);
}

TEST(ScopedTimerMetricTest, NestedTimersWorkIndependently) {
    TickSourceMock<Milliseconds> tickSource;
    DurationCounter64<Milliseconds> outerCounter;
    DurationCounter64<Milliseconds> innerCounter;

    {
        ScopedTimerMetric outerTimer(&tickSource, outerCounter);
        tickSource.advance(Milliseconds{100});
        {
            ScopedTimerMetric innerTimer(&tickSource, innerCounter);
            tickSource.advance(Milliseconds{50});
        }
        // Inner timer should have recorded 50ms.
        ASSERT_EQ(innerCounter.get(), 50);
        tickSource.advance(Milliseconds{25});
    }

    // Outer timer should have recorded the total time: 100 + 50 + 25 = 175ms.
    ASSERT_EQ(outerCounter.get(), 175);
}

TEST(ScopedTimerMetricTest, LargeDurations) {
    TickSourceMock<Milliseconds> tickSource;
    DurationCounter64<Microseconds> counter;

    {
        ScopedTimerMetric timer(&tickSource, counter);
        // Simulate a large amount of time passing.
        tickSource.advance(Seconds{10});
    }

    ASSERT_EQ(counter.get(), 10000000);
}

}  // namespace
}  // namespace mongo

