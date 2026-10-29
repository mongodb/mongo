// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/scoped_timer_metric.h"

#include "mongo/db/stats/counters.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source_mock.h"

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

    ASSERT_EQ(counter.get(), Milliseconds{100});
}

TEST(ScopedTimerMetricTest, ZeroDurationWhenNoTimeElapsed) {
    TickSourceMock<Milliseconds> tickSource;
    DurationCounter64<Milliseconds> counter;

    // No advance on read - both getTicks() calls return the same value.
    {
        ScopedTimerMetric timer(&tickSource, counter);
    }

    ASSERT_EQ(counter.get(), Milliseconds{0});
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
    ASSERT_EQ(counter.get(), Milliseconds{150});
}

TEST(ScopedTimerMetricTest, WorksWithMicroseconds) {
    TickSourceMock<Microseconds> tickSource;
    DurationCounter64<Microseconds> counter;

    tickSource.setAdvanceOnRead(Microseconds{500});

    {
        ScopedTimerMetric timer(&tickSource, counter);
    }

    ASSERT_EQ(counter.get(), Microseconds{500});
}

TEST(ScopedTimerMetricTest, DifferentDurationTypes) {
    // Use a tick source with a different duration base than the timer.
    TickSourceMock<Microseconds> tickSource;
    DurationCounter64<Milliseconds> counter;

    tickSource.setAdvanceOnRead(Microseconds{5000});

    {
        ScopedTimerMetric timer(&tickSource, counter);
    }

    ASSERT_EQ(counter.get(), Milliseconds{5});
}

TEST(ScopedTimerMetricTest, ManualAdvanceSimulatesTimePassage) {
    TickSourceMock<Milliseconds> tickSource;
    DurationCounter64<Milliseconds> counter;

    {
        ScopedTimerMetric timer(&tickSource, counter);
        // Manually advance the tick source to simulate 250ms passing.
        tickSource.advance(Milliseconds{250});
    }

    ASSERT_EQ(counter.get(), Milliseconds{250});
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
        ASSERT_EQ(innerCounter.get(), Milliseconds{50});
        tickSource.advance(Milliseconds{25});
    }

    // Outer timer should have recorded the total time: 100 + 50 + 25 = 175ms.
    ASSERT_EQ(outerCounter.get(), Milliseconds{175});
}

TEST(ScopedTimerMetricTest, LargeDurations) {
    TickSourceMock<Milliseconds> tickSource;
    DurationCounter64<Microseconds> counter;

    {
        ScopedTimerMetric timer(&tickSource, counter);
        // Simulate a large amount of time passing.
        tickSource.advance(Seconds{10});
    }

    ASSERT_EQ(counter.get(), Seconds{10});
}

}  // namespace
}  // namespace mongo
