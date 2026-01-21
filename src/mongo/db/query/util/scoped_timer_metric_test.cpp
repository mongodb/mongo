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

#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

/**
 * Simple mock counter class for testing ScopedTimerMetric.
 * Tracks the total count incremented and the number of increment calls.
 * The ScopedTimerMetric calls increment() with the raw count value (Duration::count()).
 */
class MockCounter {
public:
    void increment(int64_t count) {
        _totalCount += count;
        _incrementCount++;
    }

    int64_t getTotalCount() const {
        return _totalCount;
    }

    int getIncrementCount() const {
        return _incrementCount;
    }

    void reset() {
        _totalCount = 0;
        _incrementCount = 0;
    }

private:
    int64_t _totalCount{0};
    int _incrementCount{0};
};

TEST(ScopedTimerMetricTest, IncrementCounterOnDestruction) {
    TickSourceMock<Milliseconds> tickSource;
    MockCounter counter;

    // Set the tick source to advance by 100ms each time getTicks() is called.
    // First call (in constructor) returns 0, second call (in destructor) returns 100.
    tickSource.setAdvanceOnRead(Milliseconds{100});

    {
        ScopedTimerMetric<MockCounter, Milliseconds> timer(&tickSource, counter);
    }

    ASSERT_EQ(counter.getTotalCount(), 100);
    ASSERT_EQ(counter.getIncrementCount(), 1);
}

TEST(ScopedTimerMetricTest, ZeroDurationWhenNoTimeElapsed) {
    TickSourceMock<Milliseconds> tickSource;
    MockCounter counter;

    // No advance on read - both getTicks() calls return the same value.
    {
        ScopedTimerMetric<MockCounter, Milliseconds> timer(&tickSource, counter);
    }

    ASSERT_EQ(counter.getTotalCount(), 0);
    ASSERT_EQ(counter.getIncrementCount(), 1);
}

TEST(ScopedTimerMetricTest, MultipleTimersAccumulate) {
    TickSourceMock<Milliseconds> tickSource;
    MockCounter counter;

    tickSource.setAdvanceOnRead(Milliseconds{50});

    {
        ScopedTimerMetric<MockCounter, Milliseconds> timer1(&tickSource, counter);
    }
    {
        ScopedTimerMetric<MockCounter, Milliseconds> timer2(&tickSource, counter);
    }
    {
        ScopedTimerMetric<MockCounter, Milliseconds> timer3(&tickSource, counter);
    }

    // Each timer should add 50ms (the advance on read amount).
    ASSERT_EQ(counter.getTotalCount(), 150);
    ASSERT_EQ(counter.getIncrementCount(), 3);
}

TEST(ScopedTimerMetricTest, WorksWithMicroseconds) {
    TickSourceMock<Microseconds> tickSource;
    MockCounter counter;

    tickSource.setAdvanceOnRead(Microseconds{500});

    {
        ScopedTimerMetric<MockCounter, Microseconds> timer(&tickSource, counter);
    }

    ASSERT_EQ(counter.getTotalCount(), 500);
    ASSERT_EQ(counter.getIncrementCount(), 1);
}

TEST(ScopedTimerMetricTest, DifferentDurationTypes) {
    // Use a tick source with a different duration base than the timer.
    TickSourceMock<Microseconds> tickSource;
    MockCounter counter;

    tickSource.setAdvanceOnRead(Microseconds{5000});

    {
        ScopedTimerMetric<MockCounter, Milliseconds> timer(&tickSource, counter);
    }

    ASSERT_EQ(counter.getTotalCount(), 5);
    ASSERT_EQ(counter.getIncrementCount(), 1);
}

TEST(ScopedTimerMetricTest, ManualAdvanceSimulatesTimePassage) {
    TickSourceMock<Milliseconds> tickSource;
    MockCounter counter;

    {
        ScopedTimerMetric<MockCounter, Milliseconds> timer(&tickSource, counter);
        // Manually advance the tick source to simulate 250ms passing.
        tickSource.advance(Milliseconds{250});
    }

    ASSERT_EQ(counter.getTotalCount(), 250);
    ASSERT_EQ(counter.getIncrementCount(), 1);
}

TEST(ScopedTimerMetricTest, NestedTimersWorkIndependently) {
    TickSourceMock<Milliseconds> tickSource;
    MockCounter outerCounter;
    MockCounter innerCounter;

    {
        ScopedTimerMetric<MockCounter, Milliseconds> outerTimer(&tickSource, outerCounter);
        tickSource.advance(Milliseconds{100});
        {
            ScopedTimerMetric<MockCounter, Milliseconds> innerTimer(&tickSource, innerCounter);
            tickSource.advance(Milliseconds{50});
        }
        // Inner timer should have recorded 50ms.
        ASSERT_EQ(innerCounter.getTotalCount(), 50);
        tickSource.advance(Milliseconds{25});
    }

    // Outer timer should have recorded the total time: 100 + 50 + 25 = 175ms.
    ASSERT_EQ(outerCounter.getTotalCount(), 175);
}

TEST(ScopedTimerMetricTest, LargeDurations) {
    TickSourceMock<Milliseconds> tickSource;
    MockCounter counter;

    {
        ScopedTimerMetric<MockCounter, Milliseconds> timer(&tickSource, counter);
        // Simulate a large amount of time passing.
        tickSource.advance(Seconds{10});
    }

    ASSERT_EQ(counter.getTotalCount(), 10000);
}

}  // namespace
}  // namespace mongo

