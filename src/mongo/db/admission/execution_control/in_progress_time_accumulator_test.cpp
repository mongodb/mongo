// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/execution_control/in_progress_time_accumulator.h"

#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source_mock.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace mongo::admission::execution_control {
namespace {

// TickSourceMock<Microseconds>: 1 tick == 1 microsecond, and it only advances on explicit
// advance(), so the accumulator's clock reads are fully deterministic.
class InProgressTimeAccumulatorTest : public unittest::Test {
protected:
    TickSourceMock<Microseconds> _tickSource;
    InProgressTimeAccumulator _accumulator{&_tickSource};
};

TEST_F(InProgressTimeAccumulatorTest, NoQueueingAccruesNoTime) {
    ASSERT_EQ(_accumulator.currentCount(), 0);
    _tickSource.advance(Microseconds(1000));
    ASSERT_EQ(_accumulator.totalMicros(), 0);
    ASSERT_EQ(_accumulator.currentCount(), 0);
}

TEST_F(InProgressTimeAccumulatorTest, InProgressWaitIsVisibleBeforeItCompletes) {
    _accumulator.onCountChange(1);
    ASSERT_EQ(_accumulator.currentCount(), 1);

    // The wait is still in progress, but the elapsed time is already reflected.
    _tickSource.advance(Microseconds(500));
    ASSERT_EQ(_accumulator.totalMicros(), 500);

    _tickSource.advance(Microseconds(250));
    ASSERT_EQ(_accumulator.totalMicros(), 750);

    // Completing the wait folds the full interval and stops further accrual.
    _accumulator.onCountChange(-1);
    ASSERT_EQ(_accumulator.currentCount(), 0);
    ASSERT_EQ(_accumulator.totalMicros(), 750);

    _tickSource.advance(Microseconds(1000));
    ASSERT_EQ(_accumulator.totalMicros(), 750);
}

TEST_F(InProgressTimeAccumulatorTest, ConcurrentWaitersAreWeightedByCount) {
    _accumulator.onCountChange(1);
    _tickSource.advance(Microseconds(100));  // 1 waiter * 100us = 100

    _accumulator.onCountChange(2);           // now 3 waiters
    _tickSource.advance(Microseconds(100));  // 3 waiters * 100us = 300
    ASSERT_EQ(_accumulator.currentCount(), 3);
    ASSERT_EQ(_accumulator.totalMicros(), 100 + 300);

    _accumulator.onCountChange(-2);          // back to 1 waiter
    _tickSource.advance(Microseconds(100));  // 1 waiter * 100us = 100
    ASSERT_EQ(_accumulator.currentCount(), 1);
    ASSERT_EQ(_accumulator.totalMicros(), 100 + 300 + 100);

    _accumulator.onCountChange(-1);  // queue drains
    ASSERT_EQ(_accumulator.totalMicros(), 500);
}

TEST_F(InProgressTimeAccumulatorTest, CombinedStartedAndFinishedInOneTransition) {
    // A single recorder callback may carry both a started and a finished event; the net is what
    // matters for the waiter count.
    _accumulator.onCountChange(1);
    _tickSource.advance(Microseconds(100));

    // One operation leaves and another joins simultaneously: still one waiter throughout.
    _accumulator.onCountChange(0);
    _tickSource.advance(Microseconds(100));
    ASSERT_EQ(_accumulator.currentCount(), 1);
    ASSERT_EQ(_accumulator.totalMicros(), 200);
}

TEST_F(InProgressTimeAccumulatorTest, ReadingDoesNotDoubleCount) {
    _accumulator.onCountChange(1);
    _tickSource.advance(Microseconds(100));

    // Multiple reads of the projected value must not advance the accumulator.
    ASSERT_EQ(_accumulator.totalMicros(), 100);
    ASSERT_EQ(_accumulator.totalMicros(), 100);

    _tickSource.advance(Microseconds(100));
    _accumulator.onCountChange(-1);
    ASSERT_EQ(_accumulator.totalMicros(), 200);
}

// TickSourceMock<Nanoseconds>: 1 tick == 1 nanosecond. Sub-microsecond intervals that each floor
// to zero microseconds are preserved across folds when accumulated in ticks.
TEST(InProgressTimeAccumulatorNanosecondsTest, SubMicrosecondRemainderCarriedAcrossFolds) {
    TickSourceMock<Nanoseconds> tickSource;
    InProgressTimeAccumulator accumulator{&tickSource};

    accumulator.onCountChange(1);
    tickSource.advance(Nanoseconds(500));
    accumulator.onCountChange(0);  // fold: 500 ns accrued, still 0 µs when read
    ASSERT_EQ(accumulator.totalMicros(), 0);

    tickSource.advance(Nanoseconds(500));
    // 1000 ns accrued across two sub-microsecond folds, credited as 1 µs.
    ASSERT_EQ(accumulator.totalMicros(), 1);

    accumulator.onCountChange(-1);
    ASSERT_EQ(accumulator.totalMicros(), 1);
}

// Hammers a single accumulator from many writer and reader threads to surface data races (under
// TSAN) and to verify the documented invariants hold under concurrency. Uses the real system tick
// source so wall-clock time actually advances. Worker threads record violations in atomics rather
// than asserting directly, since a throwing ASSERT on a non-main thread would terminate the
// process.
TEST(InProgressTimeAccumulatorConcurrencyTest, ConcurrentWritersAndReaders) {
    InProgressTimeAccumulator accumulator;

    constexpr int kWriters = 8;
    constexpr int kReaders = 4;
    constexpr int kIterations = 20000;

    Atomic<bool> readersShouldStop{false};
    Atomic<bool> sawNegativeCount{false};
    Atomic<bool> sawNonMonotonicTotal{false};

    // Release all threads to hit the accumulator at once, maximizing contention/overlap.
    unittest::Barrier barrier(kWriters + kReaders);

    std::vector<stdx::thread> writers;
    for (int i = 0; i < kWriters; ++i) {
        writers.emplace_back([&] {
            barrier.countDownAndWait();
            for (int n = 0; n < kIterations; ++n) {
                // A balanced enter/exit pair: the count rises and falls, so across all writers it
                // ranges over [0, kWriters] and must settle back to 0 once every writer is done.
                accumulator.onCountChange(1);
                accumulator.onCountChange(-1);
            }
        });
    }

    std::vector<stdx::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            barrier.countDownAndWait();
            int64_t lastTotal = 0;
            while (!readersShouldStop.load()) {
                // totalMicros() is the time-integral of the count; within a single thread it must
                // never go backwards (the cache is published in mutex order off a monotonic clock).
                const int64_t total = accumulator.totalMicros();
                if (total < lastTotal) {
                    sawNonMonotonicTotal.store(true);
                }
                lastTotal = total;

                if (accumulator.currentCount() < 0) {
                    sawNegativeCount.store(true);
                }
            }
        });
    }

    for (auto& w : writers) {
        w.join();
    }
    readersShouldStop.store(true);
    for (auto& r : readers) {
        r.join();
    }

    ASSERT_FALSE(sawNegativeCount.load());
    ASSERT_FALSE(sawNonMonotonicTotal.load());
    // Every enter was matched by an exit, so nothing is left in the state.
    ASSERT_EQ(accumulator.currentCount(), 0);
    ASSERT_GTE(accumulator.totalMicros(), 0);
}

}  // namespace
}  // namespace mongo::admission::execution_control
