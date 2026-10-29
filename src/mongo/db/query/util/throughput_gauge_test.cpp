// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/throughput_gauge.h"

#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#include <vector>

namespace mongo {
namespace {

/**
 * Test fixture that provides a mock clock source for testing timing-dependent behavior of
 * ThroughputGauge.
 */
class ThroughputGaugeTest : public unittest::Test {
public:
    void setUp() override {
        _mockClock = std::make_shared<ClockSourceMock>();
        _mockClock->reset(Date_t::fromMillisSinceEpoch(1000));
    }

    ClockSourceMock* getMockClock() {
        return _mockClock.get();
    }

    Date_t now() {
        return getMockClock()->now();
    }

    void advanceTime(Milliseconds ms) {
        getMockClock()->advance(ms);
    }

private:
    std::shared_ptr<ClockSourceMock> _mockClock;
};

TEST_F(ThroughputGaugeTest, InitiallyEmpty) {
    ThroughputGauge gauge;

    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 0);
}

TEST_F(ThroughputGaugeTest, RecordSingleEvent) {
    ThroughputGauge gauge;

    gauge.recordEvent(now());

    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 1);
}

TEST_F(ThroughputGaugeTest, RecordMultipleEventsWithinOneSecond) {
    ThroughputGauge gauge;

    // Record 5 events at different times within the same second.
    gauge.recordEvent(now());
    advanceTime(Milliseconds(100));
    gauge.recordEvent(now());
    advanceTime(Milliseconds(200));
    gauge.recordEvent(now());
    advanceTime(Milliseconds(300));
    gauge.recordEvent(now());
    advanceTime(Milliseconds(300));
    gauge.recordEvent(now());

    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 5);
}

TEST_F(ThroughputGaugeTest, EventsExpireAfterOneSecond) {
    ThroughputGauge gauge;

    // Record an event at T=0.
    gauge.recordEvent(now());
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 1);

    // Advance time by just under 1 second - event should still be counted.
    advanceTime(Milliseconds(999));
    gauge.recordEvent(now());
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 2);

    // Advance time by more than 1 second from first event - first event should expire.
    advanceTime(Milliseconds(2));
    gauge.recordEvent(now());
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 2);  // Only last 2 events.
}

TEST_F(ThroughputGaugeTest, AllEventsExpire) {
    ThroughputGauge gauge;

    for (int i = 0; i < 5; i++) {
        gauge.recordEvent(now());
        advanceTime(Milliseconds(100));
    }
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 5);

    // Advance time by more than 1 second - all events should expire.
    advanceTime(Seconds(2));
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 0);
    gauge.recordEvent(now());
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 1);  // Only the new event.
}

TEST_F(ThroughputGaugeTest, SlidingWindowBehavior) {
    ThroughputGauge gauge;

    // Record events at T=0, T=500ms, T=1000ms, T=1500ms.
    auto startTime = now();
    gauge.recordEvent(startTime);

    advanceTime(Milliseconds(500));
    gauge.recordEvent(now());

    advanceTime(Milliseconds(500));
    gauge.recordEvent(now());
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 3);

    advanceTime(Milliseconds(500));
    gauge.recordEvent(now());
    // At T=1500ms, event at T=0 is > 1 second old (1500ms), so it's expired
    // Events remaining: T=500, T=1000, T=1500
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 3);

    // At T=2000ms, T=500 expires (1500ms old), but T=1000 stays (exactly 1000ms old, not > 1000ms)
    // Events remaining: T=1000, T=1500, T=2000
    advanceTime(Milliseconds(500));
    gauge.recordEvent(now());
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 3);
}

TEST_F(ThroughputGaugeTest, ConcurrentRecordEvents) {
    ThroughputGauge gauge;
    constexpr int numThreads = 10;
    constexpr int eventsPerThread = 100;

    std::vector<stdx::thread> threads;
    Atomic<unsigned> ready{0};

    // Each thread will record events.
    for (int i = 0; i < numThreads; i++) {
        threads.emplace_back([&, i]() {
            // Busy-wait until all threads are ready.
            ready.fetchAndAdd(1);
            while (ready.load() < numThreads) {
            }

            // Record events.
            for (int j = 0; j < eventsPerThread; j++) {
                gauge.recordEvent(now());
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All events should be recorded.
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), numThreads * eventsPerThread);
}

TEST_F(ThroughputGaugeTest, ConcurrentRecordAndRead) {
    ThroughputGauge gauge;
    constexpr int numWriteThreads = 5;
    constexpr int numReadThreads = 5;
    constexpr int eventsPerThread = 50;

    std::vector<stdx::thread> threads;
    Atomic<unsigned> ready{0};
    Atomic<bool> stopReading{false};

    // Writer threads.
    for (int i = 0; i < numWriteThreads; i++) {
        threads.emplace_back([&]() {
            // Busy-wait until all threads are ready.
            ready.fetchAndAdd(1);
            while (ready.load() < (numWriteThreads + numReadThreads)) {
            }

            for (int j = 0; j < eventsPerThread; j++) {
                gauge.recordEvent(now());
            }
        });
    }

    // Reader threads.
    for (int i = 0; i < numReadThreads; i++) {
        threads.emplace_back([&]() {
            // Busy-wait until all threads are ready.
            ready.fetchAndAdd(1);
            while (ready.load() < (numWriteThreads + numReadThreads)) {
            }

            // Continuously read while writers are working.
            while (!stopReading.load()) {
                auto count = gauge.nEventsInPreviousSecond(now());
                ASSERT_LESS_THAN_OR_EQUALS(count, numWriteThreads * eventsPerThread);
            }
        });
    }

    // Wait for all threads to complete.
    for (int i = 0; i < numWriteThreads; i++) {
        threads[i].join();
    }
    stopReading.store(true);
    for (int i = numWriteThreads; i < numWriteThreads + numReadThreads; i++) {
        threads[i].join();
    }

    // Verify final count.
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), numWriteThreads * eventsPerThread);
}

TEST_F(ThroughputGaugeTest, ConcurrentRecordWithExpiration) {
    ThroughputGauge gauge;
    constexpr int numThreads = 8;
    constexpr int eventsPerThread = 25;

    std::vector<stdx::thread> threads;
    Atomic<unsigned> ready{0};

    auto startTime = now();

    // Phase 1: All threads record events at startTime
    for (int i = 0; i < numThreads; i++) {
        threads.emplace_back([&]() {
            ready.fetchAndAdd(1);
            while (ready.load() < numThreads) {
            }

            for (int j = 0; j < eventsPerThread; j++) {
                gauge.recordEvent(startTime);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), numThreads * eventsPerThread);

    // Phase 2: Advance time and record new events, old ones should expire.
    advanceTime(Milliseconds(1100));
    auto newTime = now();

    threads.clear();
    ready.store(0);

    for (int i = 0; i < numThreads; i++) {
        threads.emplace_back([&]() {
            ready.fetchAndAdd(1);
            while (ready.load() < numThreads) {
            }

            for (int j = 0; j < eventsPerThread; j++) {
                gauge.recordEvent(newTime);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Only new events should be counted.
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), numThreads * eventsPerThread);
}


TEST_F(ThroughputGaugeTest, EventsAtExactOneSecondBoundary) {
    ThroughputGauge gauge;

    auto startTime = now();
    gauge.recordEvent(startTime);

    // Record event at exactly 1 second later.
    advanceTime(Seconds(1));
    gauge.recordEvent(now());

    // The 1 second window is inclusive, so the old event should still be there.
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 2);

    // Record event at 1 second + 1 millisecond.
    advanceTime(Milliseconds(1));
    gauge.recordEvent(now());

    // Now the first event should be expired.
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 2);
}

TEST_F(ThroughputGaugeTest, InterleaveRecordAndExpire) {
    ThroughputGauge gauge;

    // Record events at T=0, T=100, T=200.
    gauge.recordEvent(now());
    advanceTime(Milliseconds(100));
    gauge.recordEvent(now());
    advanceTime(Milliseconds(100));
    gauge.recordEvent(now());
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 3);

    // Advance to T=1100 and record - first event should expire.
    advanceTime(Milliseconds(900));
    gauge.recordEvent(now());
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 3);  // Events at T=100, T=200, T=1100.

    // Advance to T=1200 and record - T=100 event should expire.
    advanceTime(Milliseconds(100));
    gauge.recordEvent(now());
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 3);  // Events at T=200, T=1100, T=1200.

    // Advance to T=1300 and record - T=200 event should expire.
    advanceTime(Milliseconds(100));
    gauge.recordEvent(now());
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 3);  // Events at T=1100, T=1200, T=1300.
}

TEST_F(ThroughputGaugeTest, EventsExpireWhenThroughputIsExamined) {
    ThroughputGauge gauge;

    // Record events at T=0, T=100, T=200.
    gauge.recordEvent(now());
    advanceTime(Milliseconds(100));
    gauge.recordEvent(now());
    advanceTime(Milliseconds(100));
    gauge.recordEvent(now());
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 3);

    // Advance to T=1100 and check - first event should expire.
    advanceTime(Milliseconds(900));
    // Note: NOT calling gauge.recordEvent().
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 2);  // Events at T=100, T=200.

    // Advance to T=1200 and re-check - T=100 event should expire.
    advanceTime(Milliseconds(100));
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 1);  // Event at T=200.

    // Advance to T=1300 and re-check - T=200 event should expire.
    advanceTime(Milliseconds(100));
    ASSERT_EQ(gauge.nEventsInPreviousSecond(now()), 0);
}

}  // namespace
}  // namespace mongo
