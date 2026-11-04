// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/observability/metrics_primitives.h"

#include "mongo/base/counter.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/stats/operation_latency_histogram.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mongo::observability {
namespace {

// A controllable steady-clock stand-in for the timed-reset tests, so window expiry is exercised
// deterministically with no sleeps.
struct FakeClock {
    using duration = std::chrono::steady_clock::duration;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<FakeClock, duration>;
    static constexpr bool is_steady = true;

    static time_point now() {
        return time_point(_now);
    }

    static void advance(duration delta) {
        _now += delta;
    }

    static void reset() {
        _now = std::chrono::duration_cast<duration>(std::chrono::seconds(1000));
    }

    static duration _now;
};

FakeClock::duration FakeClock::_now =
    std::chrono::duration_cast<FakeClock::duration>(std::chrono::seconds(1000));

TEST(MetricsPrimitivesTest, CounterIsMonotonic) {
    Counter<std::int64_t> c;
    ASSERT_EQ(c(), 0);
    c.add(2);
    c.add(3);
    ASSERT_EQ(c(), 5);
    c.add(10);
    ASSERT_EQ(c(), 15);
}

TEST(MetricsPrimitivesTest, UpDownCounterAllowsNegativeAdd) {
    UpDownCounter<std::int64_t> c;
    c.add(5);
    ASSERT_EQ(c(), 5);
    c.add(-2);
    ASSERT_EQ(c(), 3);
    c.add(-10);
    ASSERT_EQ(c(), -7);
}

TEST(MetricsPrimitivesTest, UpDownCounterReturnsToZeroAfterBalancingAdds) {
    UpDownCounter<std::int64_t> c;
    c.add(3);
    ASSERT_EQ(c(), 3);
    c.add(-3);
    ASSERT_EQ(c(), 0);
}

TEST(MetricsPrimitivesTest, GaugeRendersDefaultUntilSet) {
    // A bare Gauge has no written-tracking; it reads back its default (0) until the first set().
    Gauge<std::int64_t> g;
    ASSERT_EQ(g(), 0);
    g.set(42);
    ASSERT_EQ(g(), 42);
    g.set(7);
    ASSERT_EQ(g(), 7);
}

TEST(MetricsPrimitivesTest, PrimitivesExposeValueType) {
    static_assert(std::is_same_v<Counter<std::int64_t>::value_type, std::int64_t>);
    static_assert(std::is_same_v<UpDownCounter<std::int64_t>::value_type, std::int64_t>);
    static_assert(std::is_same_v<Gauge<double>::value_type, double>);
    static_assert(std::is_same_v<HighWaterMark<std::int64_t>::value_type, std::int64_t>);
    static_assert(std::is_same_v<TimedHighWaterMark<std::int64_t>::value_type, std::int64_t>);
    static_assert(std::is_same_v<AtomicHistogram<double>::value_type, double>);
    static_assert(std::is_same_v<Histogram<double>::value_type, double>);
    static_assert(std::is_same_v<SynchronizedValue<std::string>::value_type, std::string>);
}

TEST(MetricsPrimitivesTest, CounterConcurrentAdd) {
    Counter<std::int64_t> c;
    constexpr int kThreads = 2;
    constexpr int kIters = 100000;
    std::vector<stdx::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&c] {
            for (int i = 0; i < kIters; ++i) {
                c.add(1);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    ASSERT_EQ(c(), std::int64_t{kThreads} * kIters);
}

TEST(MetricsPrimitivesTest, UpDownCounterConcurrentAdd) {
    UpDownCounter<std::int64_t> c;
    constexpr int kThreads = 2;
    constexpr int kIters = 100000;
    std::vector<stdx::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&c, t] {
            for (int i = 0; i < kIters; ++i) {
                c.add(t == 0 ? 1 : -1);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    ASSERT_EQ(c(), 0);
}

TEST(MetricsPrimitivesTest, CounterDouble) {
    Counter<double> c;
    ASSERT_EQ(c(), 0.0);
    c.add(1.5);
    c.add(2.25);
    ASSERT_EQ(c(), 3.75);
}

TEST(MetricsPrimitivesTest, CounterRejectsNegativeIncrement) {
    Counter<std::int64_t> c;
    c.add(5);
    unittest::TassertGuard tassertGuard;
    ASSERT_THROWS_CODE_AND_WHAT(c.add(-1),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "Counter increment must be nonnegative");
    // The rejected increment left the value untouched.
    ASSERT_EQ(c(), 5);
}

TEST(MetricsPrimitivesTest, CounterAcceptsZeroAndPositiveIncrements) {
    Counter<std::int64_t> c;
    c.add(0);
    ASSERT_EQ(c(), 0);
    c.add(4);
    ASSERT_EQ(c(), 4);
}

TEST(MetricsPrimitivesTest, GaugeDouble) {
    Gauge<double> g;
    ASSERT_EQ(g(), 0.0);
    g.set(3.5);
    ASSERT_EQ(g(), 3.5);
}

// Side-by-side parity with the legacy primitives: the new type must read back the same value as
// the mongo:: type it replaces after every step of the same sequence.
TEST(MetricsPrimitivesTest, CounterMatchesLegacyCounter64) {
    Counter<std::int64_t> counter;
    mongo::Counter64 legacy;
    for (const std::int64_t delta : {1, 5, 10, 100}) {
        counter.add(delta);
        legacy.incrementRelaxed(delta);
        ASSERT_EQ(counter(), static_cast<std::int64_t>(legacy.get()));
    }
}

TEST(MetricsPrimitivesTest, GaugeMatchesLegacyAtomic64MetricSet) {
    Gauge<std::int64_t> gauge;
    mongo::Atomic64Metric legacy;
    for (const std::int64_t value : {3, -7, 0, 42, -1}) {
        gauge.set(value);
        legacy.set(value);
        ASSERT_EQ(gauge(), legacy.get());
    }
}

TEST(MetricsPrimitivesTest, HighWaterMarkMatchesLegacyAtomic64MetricSetToMax) {
    HighWaterMark<std::int64_t> highWaterMark;
    mongo::Atomic64Metric legacy;
    // An out-of-order sequence: smaller readings must be ignored by both types.
    for (const std::int64_t value : {10, 4, 7, 11, 11, 3, 25, -5}) {
        highWaterMark.setToMax(value);
        legacy.setToMax(value);
        ASSERT_EQ(highWaterMark(), legacy.get());
    }
}

TEST(MetricsPrimitivesTest, HighWaterMarkMaxOnlyProgression) {
    HighWaterMark<std::int64_t> h;
    ASSERT_EQ(h(), 0);
    h.setToMax(5);
    ASSERT_EQ(h(), 5);
    h.setToMax(9);
    ASSERT_EQ(h(), 9);
    h.setToMax(9);
    ASSERT_EQ(h(), 9);
}

TEST(MetricsPrimitivesTest, HighWaterMarkIgnoresOutOfOrderRecords) {
    HighWaterMark<std::int64_t> h;
    h.setToMax(10);
    h.setToMax(4);
    ASSERT_EQ(h(), 10);
    h.setToMax(7);
    ASSERT_EQ(h(), 10);
    h.setToMax(11);
    ASSERT_EQ(h(), 11);
}

TEST(MetricsPrimitivesTest, HighWaterMarkResetLowersBaselineAndRegrows) {
    HighWaterMark<std::int64_t> h;
    h.setToMax(10);
    h.reset(3);
    ASSERT_EQ(h(), 3);
    h.setToMax(5);
    ASSERT_EQ(h(), 5);
    h.setToMax(1);
    ASSERT_EQ(h(), 5);
    h.setToMax(20);
    ASSERT_EQ(h(), 20);
}

TEST(MetricsPrimitivesTest, HighWaterMarkResetToCurrentReadingKeepsValue) {
    HighWaterMark<std::int64_t> h;
    h.setToMax(42);
    h.reset(h());
    ASSERT_EQ(h(), 42);
}

TEST(MetricsPrimitivesTest, TimedHighWaterMarkResetGrowsWithinWindow) {
    using H = TimedHighWaterMark<std::int64_t, FakeClock>;
    FakeClock::reset();
    H h;
    h.resetAfter(std::chrono::milliseconds(100));
    h.setToMax(10);
    ASSERT_EQ(h(), 10);
    // Partway through the window a smaller record is a no-op: the window has not expired, so the
    // maximum is retained rather than rolled down to the incoming value.
    FakeClock::advance(std::chrono::milliseconds(40));
    h.setToMax(5);
    ASSERT_EQ(h(), 10);
    // Still inside the same window (80 ms < 100 ms); a larger record grows the maximum.
    FakeClock::advance(std::chrono::milliseconds(40));
    h.setToMax(20);
    ASSERT_EQ(h(), 20);
    h.setToMax(6);
    ASSERT_EQ(h(), 20);
}

TEST(MetricsPrimitivesTest, TimedHighWaterMarkResetRestartsWindowOnExpiry) {
    using H = TimedHighWaterMark<std::int64_t, FakeClock>;
    FakeClock::reset();
    H h;
    h.resetAfter(std::chrono::milliseconds(100));
    h.setToMax(10);
    ASSERT_EQ(h(), 10);
    FakeClock::advance(std::chrono::milliseconds(150));
    // The window expired, so the first record after it restarts the window from the incoming value
    // rather than keeping the old maximum.
    h.setToMax(5);
    ASSERT_EQ(h(), 5);
    // A larger record grows the fresh window.
    h.setToMax(7);
    ASSERT_EQ(h(), 7);
}

TEST(MetricsPrimitivesTest, TimedHighWaterMarkManualResetLowersMarkPastWindowExpiry) {
    using H = TimedHighWaterMark<std::int64_t, FakeClock>;
    FakeClock::reset();
    H h;
    h.resetAfter(std::chrono::milliseconds(100));
    h.setToMax(100);
    // Manual reset partway through the window.
    FakeClock::advance(std::chrono::milliseconds(20));
    h.reset(5);
    ASSERT_EQ(h(), 5);
    // Let the window the reset started expire, then record below the pre-reset maximum.
    FakeClock::advance(std::chrono::milliseconds(100));
    h.setToMax(20);
    // The pre-reset maximum (100) must not resurface.
    ASSERT_EQ(h(), 20);
}

TEST(MetricsPrimitivesTest, TimedHighWaterMarkManualResetStartsFreshWindow) {
    using H = TimedHighWaterMark<std::int64_t, FakeClock>;
    FakeClock::reset();
    H h;
    h.resetAfter(std::chrono::milliseconds(100));
    h.setToMax(10);  // The window starts at t = 0.
    ASSERT_EQ(h(), 10);
    // The reset lowers the value and restarts the window at t = 60 ms (which now expires at
    // t = 160 ms).
    FakeClock::advance(std::chrono::milliseconds(60));
    h.reset(3);
    ASSERT_EQ(h(), 3);
    // Land past the original window's expiry (t = 100 ms) but inside the window the reset started.
    // If reset() restarted the window, the smaller reading below is an in-window no-op and the
    // baseline of 3 is retained. If reset() failed to restart the window, this point would look
    // expired, the reading would roll the window down to 2, and the assertion would fail.
    FakeClock::advance(std::chrono::milliseconds(50));
    h.setToMax(2);
    ASSERT_EQ(h(), 3);
    h.setToMax(4);
    ASSERT_EQ(h(), 4);
}

TEST(MetricsPrimitivesTest, TimedHighWaterMarkResetIsReadOnlyOnRead) {
    using H = TimedHighWaterMark<std::int64_t, FakeClock>;
    FakeClock::reset();
    H h;
    h.resetAfter(std::chrono::milliseconds(100));
    h.setToMax(10);
    FakeClock::advance(std::chrono::milliseconds(150));
    // Reading does not roll the window, so the stale maximum is still reported.
    ASSERT_EQ(h(), 10);
    // The first setToMax after expiry is what rolls the window.
    h.setToMax(1);
    ASSERT_EQ(h(), 1);
}

TEST(MetricsPrimitivesTest, TimedHighWaterMarkReenablingTimedResetStartsFreshWindow) {
    using H = TimedHighWaterMark<std::int64_t, FakeClock>;
    FakeClock::reset();
    H h;
    h.resetAfter(std::chrono::milliseconds(100));
    h.setToMax(10);
    ASSERT_EQ(h(), 10);
    // Disabling time-based reset leaves a plain cumulative maximum.
    h.resetAfter(std::chrono::steady_clock::duration::zero());
    h.setToMax(5);
    ASSERT_EQ(h(), 10);
    h.setToMax(100);
    ASSERT_EQ(h(), 100);
    // Re-enabling timed reset must start from the next observation, not the stale window start.
    h.resetAfter(std::chrono::milliseconds(100));
    h.setToMax(1);
    ASSERT_EQ(h(), 1);
    FakeClock::advance(std::chrono::milliseconds(40));
    h.setToMax(200);
    ASSERT_EQ(h(), 200);
}

TEST(MetricsPrimitivesTest, TimedHighWaterMarkFirstRecordAtClockEpochStartsWindow) {
    using H = TimedHighWaterMark<std::int64_t, FakeClock>;
    FakeClock::reset();
    // A window beginning at the clock's epoch has a zero timestamp; it still counts as begun.
    FakeClock::advance(std::chrono::seconds(-1000));
    H h;
    h.resetAfter(std::chrono::milliseconds(100));
    h.setToMax(10);
    ASSERT_EQ(h(), 10);
    FakeClock::advance(std::chrono::milliseconds(40));
    h.setToMax(5);
    ASSERT_EQ(h(), 10);
    FakeClock::advance(std::chrono::milliseconds(60));
    h.setToMax(3);
    ASSERT_EQ(h(), 3);
}

TEST(MetricsPrimitivesTest, TimedHighWaterMarkRolloverKeepsInWindowMax) {
    // A rollover lowers the value unconditionally, so an unsynchronized rollover can erase an
    // in-window update that lands between publishing the new window start and seeding the value.
    // Each round expires the window and has every thread race to record; once all have finished,
    // the largest recorded value must be present.
    using H = TimedHighWaterMark<std::int64_t, FakeClock>;
    FakeClock::reset();
    H h;
    h.resetAfter(std::chrono::milliseconds(100));

    constexpr int kThreads = 4;
    constexpr int kRounds = 200;
    std::atomic<int> round{-1};
    std::atomic<int> doneCount{0};
    std::atomic<bool> shutdown{false};

    std::vector<stdx::thread> threads;
    threads.reserve(kThreads);
    for (int t = 1; t <= kThreads; ++t) {
        threads.emplace_back([&, t] {
            int lastRound = -1;
            for (;;) {
                while (round.load() == lastRound && !shutdown.load()) {
                    std::this_thread::yield();
                }
                if (shutdown.load()) {
                    return;
                }
                lastRound = round.load();
                h.setToMax(t);
                doneCount.fetch_add(1);
            }
        });
    }

    for (int r = 0; r < kRounds; ++r) {
        round.store(r);
        while (doneCount.load() < kThreads) {
            std::this_thread::yield();
        }
        // All threads have finished this round's recordings, so no clock read overlaps with the
        // advance.
        FakeClock::advance(std::chrono::milliseconds(100));
        // The window expired, so the round's maximum must have won the rollover race.
        ASSERT_EQ(h(), kThreads);
        doneCount.store(0);
    }

    shutdown.store(true);
    for (auto& thread : threads) {
        thread.join();
    }
}

TEST(MetricsPrimitivesTest, HighWaterMarkDouble) {
    HighWaterMark<double> h;
    ASSERT_EQ(h(), 0.0);
    h.setToMax(1.5);
    ASSERT_EQ(h(), 1.5);
    h.setToMax(0.25);
    ASSERT_EQ(h(), 1.5);
    h.reset(0.5);
    ASSERT_EQ(h(), 0.5);
    h.setToMax(2.75);
    ASSERT_EQ(h(), 2.75);
}

// The AtomicHistogram's bucketing must agree with OperationLatencyHistogram, whose
// makeOperationLatencyBucketBoundaries() supplies the same edges as explicit OTel boundaries.
// getLowerBounds() returns the inclusive lower bound of every legacy bucket, so a value belongs to
// the last bucket whose lower bound is <= value.
TEST(MetricsPrimitivesTest, AtomicHistogramMatchesOperationLatencyBucketing) {
    const auto boundaries =
        operation_latency_histogram_details::makeOperationLatencyBucketBoundaries();
    const auto lowerBounds = operation_latency_histogram_details::getLowerBounds();

    // Probe values cover the region below the first boundary, every boundary together with its
    // immediate neighbours, and the saturation region above the last boundary.
    const std::vector<std::uint64_t> probes = {0,
                                               1,
                                               2,
                                               3,
                                               4,
                                               5,
                                               7,
                                               8,
                                               15,
                                               16,
                                               31,
                                               32,
                                               1023,
                                               1024,
                                               2047,
                                               2048,
                                               2049,
                                               3071,
                                               3072,
                                               3073,
                                               4095,
                                               4096,
                                               6143,
                                               6144,
                                               8191,
                                               8192,
                                               65535,
                                               65536,
                                               98303,
                                               98304,
                                               98305,
                                               131071,
                                               131072,
                                               2097151,
                                               2097152,
                                               2097153,
                                               4194303,
                                               4194304,
                                               1000000000,
                                               1099511627775,
                                               1099511627776,
                                               2000000000000};

    for (const auto value : probes) {
        AtomicHistogram<std::uint64_t> histogram(boundaries);
        histogram.observe(value);
        const auto snapshot = histogram.snapshot();
        ASSERT_EQ(snapshot.count, 1u) << "value " << value;

        const auto expected = static_cast<std::size_t>(
                                  std::upper_bound(lowerBounds.begin(), lowerBounds.end(), value) -
                                  lowerBounds.begin()) -
            1;

        std::size_t observed = 0;
        for (std::size_t i = 0; i < snapshot.buckets.size(); ++i) {
            if (snapshot.buckets[i] != 0) {
                observed = i;
            }
        }
        ASSERT_EQ(observed, expected) << "value " << value;
        // The value falls inside the bucket it was counted into: the bucket's inclusive lower
        // bound is <= value, and value is below the next bucket's lower bound.
        ASSERT_LE(lowerBounds[observed], value) << "value " << value;
        if (observed + 1 < lowerBounds.size()) {
            ASSERT_LT(value, lowerBounds[observed + 1]) << "value " << value;
        }
    }
}

TEST(MetricsPrimitivesTest, AtomicHistogramBucketsOnInclusiveLowerBounds) {
    // bucket 0: (-inf, 10), bucket 1: [10, 20), bucket 2: [20, 30), bucket 3: [30, +inf).
    AtomicHistogram<std::int64_t> histogram({10.0, 20.0, 30.0});
    histogram.observe(9);
    histogram.observe(10);
    histogram.observe(19);
    histogram.observe(20);
    histogram.observe(29);
    histogram.observe(30);
    histogram.observe(31);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 7u);
    ASSERT_EQ(snapshot.buckets.size(), 4u);
    ASSERT_EQ(snapshot.buckets[0], 1u);  // 9
    ASSERT_EQ(snapshot.buckets[1], 2u);  // 10, 19
    ASSERT_EQ(snapshot.buckets[2], 2u);  // 20, 29
    ASSERT_EQ(snapshot.buckets[3], 2u);  // 30, 31
    ASSERT_EQ(snapshot.sum, 9u + 10u + 19u + 20u + 29u + 30u + 31u);
}

TEST(MetricsPrimitivesTest, AtomicHistogramSaturatesAboveLastBoundary) {
    AtomicHistogram<std::uint64_t> histogram({1.0, 2.0, 3.0});
    histogram.observe(3);  // Exactly the last boundary.
    histogram.observe(4);
    histogram.observe(1000000);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 3u);
    ASSERT_EQ(snapshot.buckets.size(), 4u);
    ASSERT_EQ(snapshot.buckets[3], 3u);
    ASSERT_EQ(snapshot.sum, 3u + 4u + 1000000u);
}

TEST(MetricsPrimitivesTest, BucketBoundariesRejectNan) {
    // NaN never compares ordered, so std::is_sorted accepts {1.0, NaN, 2.0}; the constructor
    // screens for NaN explicitly because such a boundary would violate std::upper_bound's
    // partition precondition.
    const std::vector<double> boundaries{1.0, std::numeric_limits<double>::quiet_NaN(), 2.0};
    unittest::TassertGuard tassertGuard;
    ASSERT_THROWS_CODE_AND_WHAT(BucketBoundaries<std::int64_t>(boundaries),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "Histogram boundaries must not contain NaN");
}

TEST(MetricsPrimitivesTest, BucketBoundariesRejectUnsorted) {
    unittest::TassertGuard tassertGuard;
    ASSERT_THROWS_CODE_AND_WHAT(BucketBoundaries<std::int64_t>({2.0, 1.0}),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "Histogram boundaries must be sorted");
}

TEST(MetricsPrimitivesTest, AtomicHistogramBucketsIntegralObservationAt2Pow63) {
    // 0x1p63 (2^63) is strictly greater than every int64_t. Converting INT64_MAX to double would
    // round it up to 2^63 and push the observation into the upper bucket; it belongs below.
    AtomicHistogram<std::int64_t> histogram({0x1p63});
    histogram.observe(std::numeric_limits<std::int64_t>::max());
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.buckets[0], 1u);
    ASSERT_EQ(snapshot.buckets[1], 0u);
}

TEST(MetricsPrimitivesTest, AtomicHistogramBucketsIntegralObservationAt2Pow62) {
    // 2^62 is exactly representable, but the neighbouring int64_t values are not: 2^62 - 1, 2^62
    // and 2^62 + 1 all round to the same double. The comparison must still separate them.
    const std::int64_t boundary = std::int64_t{1} << 62;
    AtomicHistogram<std::int64_t> histogram({static_cast<double>(boundary)});
    histogram.observe(boundary - 1);
    histogram.observe(boundary);
    histogram.observe(boundary + 1);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.buckets[0], 1u);  // boundary - 1
    ASSERT_EQ(snapshot.buckets[1], 2u);  // boundary, boundary + 1
}

TEST(MetricsPrimitivesTest, HistogramBucketsLargeIntegralBoundariesExactly) {
    // The mutex-guarded variant shares BucketBoundaries, so it must bucket identically.
    const std::int64_t boundary = std::int64_t{1} << 62;
    Histogram<std::int64_t> histogram({static_cast<double>(boundary)});
    histogram.observe(boundary - 1);
    histogram.observe(boundary);
    histogram.observe(boundary + 1);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.buckets[0], 1u);  // boundary - 1
    ASSERT_EQ(snapshot.buckets[1], 2u);  // boundary, boundary + 1
}

TEST(MetricsPrimitivesTest, AtomicHistogramBucketsIntegralObservationAgainstFractionalBoundary) {
    // A non-integral boundary admits equality, so integral observations follow floor semantics:
    // 2 and below stay in bucket 0, 3 and above move up.
    AtomicHistogram<std::int64_t> histogram({2.5});
    histogram.observe(2);
    histogram.observe(3);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.buckets[0], 1u);  // 2
    ASSERT_EQ(snapshot.buckets[1], 1u);  // 3
}

TEST(MetricsPrimitivesTest, AtomicHistogramBucketsUnsignedObservationAt2Pow63) {
    const std::uint64_t boundary = std::uint64_t{1} << 63;
    AtomicHistogram<std::uint64_t> histogram({static_cast<double>(boundary)});
    histogram.observe(boundary - 1);
    histogram.observe(boundary);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.buckets[0], 1u);  // boundary - 1 rounds up to boundary as a double
    ASSERT_EQ(snapshot.buckets[1], 1u);  // boundary
}

TEST(MetricsPrimitivesTest, AtomicHistogramBucketsUnsignedObservationAt2Pow64) {
    // kFirstDoubleAboveUint64 (2^64) is strictly greater than every uint64_t.
    AtomicHistogram<std::uint64_t> histogram({kFirstDoubleAboveUint64});
    histogram.observe(std::numeric_limits<std::uint64_t>::max());
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.buckets[0], 1u);
    ASSERT_EQ(snapshot.buckets[1], 0u);
}

TEST(MetricsPrimitivesTest, AtomicHistogramBucketsLongDoubleObservationJustBelowBoundary) {
    // Casting a long double observation to double would round it onto the boundary it is strictly
    // below, moving it into the upper bucket.
    const long double justBelow = std::nextafter(1.0L, 0.0L);
    AtomicHistogram<long double> histogram({1.0});
    histogram.observe(justBelow);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.buckets[0], 1u);
    ASSERT_EQ(snapshot.buckets[1], 0u);

    // The mutex-guarded variant shares BucketBoundaries, so it must bucket identically.
    Histogram<long double> guarded({1.0});
    guarded.observe(justBelow);
    ASSERT_EQ(guarded.snapshot().buckets[0], 1u);
    ASSERT_EQ(guarded.snapshot().buckets[1], 0u);
}

TEST(MetricsPrimitivesTest, AtomicHistogramSaturatesAboveUint64RangeInSum) {
    // kFirstDoubleAboveUint64 is out of range for the unsigned sum; the contribution saturates
    // rather than invoking undefined behaviour in the cast.
    AtomicHistogram<double> histogram({10.0});
    histogram.observe(kFirstDoubleAboveUint64);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 1u);
    ASSERT_EQ(snapshot.sum, std::numeric_limits<std::uint64_t>::max());
    ASSERT_EQ(snapshot.buckets[1], 1u);
}

TEST(MetricsPrimitivesTest, AtomicHistogramSaturatesInfinityInSum) {
    AtomicHistogram<double> histogram({10.0});
    histogram.observe(std::numeric_limits<double>::infinity());
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 1u);
    ASSERT_EQ(snapshot.sum, std::numeric_limits<std::uint64_t>::max());
    ASSERT_EQ(snapshot.buckets[1], 1u);
}

TEST(MetricsPrimitivesTest, AtomicHistogramSnapshotShape) {
    AtomicHistogram<std::int64_t> unbucketed;
    const auto unbucketedSnapshot = unbucketed.snapshot();
    ASSERT_EQ(unbucketedSnapshot.count, 0u);
    ASSERT_EQ(unbucketedSnapshot.sum, 0u);
    ASSERT_EQ(unbucketedSnapshot.buckets.size(), 1u);

    AtomicHistogram<std::int64_t> histogram({1.0, 2.0, 3.0, 4.0});
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 0u);
    ASSERT_EQ(snapshot.sum, 0u);
    ASSERT_EQ(snapshot.buckets.size(), 5u);
    for (const auto bucket : snapshot.buckets) {
        ASSERT_EQ(bucket, 0u);
    }
}

TEST(MetricsPrimitivesTest, AtomicHistogramTruncatesFractionalObservationsInSum) {
    AtomicHistogram<double> histogram({10.0});
    histogram.observe(2.9);
    histogram.observe(0.5);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 2u);
    ASSERT_EQ(snapshot.sum, 2u);  // 2.9 truncates to 2, 0.5 truncates to 0.
    ASSERT_EQ(snapshot.buckets[0], 2u);
}

TEST(MetricsPrimitivesTest, AtomicHistogramClampsNegativeFloatingObservationsInSum) {
    AtomicHistogram<double> histogram({10.0});
    histogram.observe(-2.5);
    histogram.observe(3.5);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 2u);
    // -2.5 still counts and buckets, but contributes 0 to the unsigned sum; 3.5 truncates to 3.
    ASSERT_EQ(snapshot.sum, 3u);
    ASSERT_EQ(snapshot.buckets[0], 2u);
}

TEST(MetricsPrimitivesTest, AtomicHistogramNegativeIntegerObservationsWrapInSum) {
    AtomicHistogram<std::int64_t> histogram({10.0});
    histogram.observe(-1);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 1u);
    // A negative integral value converts to the unsigned sum by the usual modular rule.
    ASSERT_EQ(snapshot.sum, std::numeric_limits<std::uint64_t>::max());
    ASSERT_EQ(snapshot.buckets[0], 1u);
}

TEST(MetricsPrimitivesTest, HistogramsExposeBoundaries) {
    const std::vector<double> boundaries = {1.0, 2.0, 3.0};
    AtomicHistogram<std::int64_t> atomic(boundaries);
    Histogram<std::int64_t> mutexGuarded(boundaries);
    for (const auto& histogram : {&atomic.boundaries(), &mutexGuarded.boundaries()}) {
        ASSERT_EQ(histogram->size(), boundaries.size());
        for (std::size_t i = 0; i < boundaries.size(); ++i) {
            ASSERT_EQ((*histogram)[i], boundaries[i]) << "boundary " << i;
        }
    }
}

TEST(MetricsPrimitivesTest, AtomicHistogramConcurrentObserveIsExact) {
    AtomicHistogram<std::int64_t> histogram({10.0});
    constexpr int kThreads = 2;
    constexpr int kIters = 100000;
    std::vector<stdx::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&histogram, t] {
            // Thread 0 lands in bucket 0, thread 1 in bucket 1.
            const std::int64_t value = t == 0 ? 5 : 15;
            for (int i = 0; i < kIters; ++i) {
                histogram.observe(value);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    // Every field is a fetch-add, so once the threads have joined the totals are exact.
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, std::uint64_t{kThreads} * kIters);
    ASSERT_EQ(snapshot.buckets[0], std::uint64_t{kIters});
    ASSERT_EQ(snapshot.buckets[1], std::uint64_t{kIters});
    ASSERT_EQ(snapshot.sum, std::uint64_t{kIters} * (5u + 15u));
    std::uint64_t bucketed = 0;
    for (const auto bucket : snapshot.buckets) {
        bucketed += bucket;
    }
    ASSERT_EQ(bucketed, snapshot.count);
}

TEST(MetricsPrimitivesTest, HistogramExactCountSumMinMaxAndBuckets) {
    Histogram<std::int64_t> histogram({10.0, 20.0, 30.0});
    histogram.observe(5);
    histogram.observe(15);
    histogram.observe(25);
    histogram.observe(35);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 4u);
    ASSERT_EQ(snapshot.sum, 80.0);
    ASSERT_EQ(*snapshot.min, 5.0);
    ASSERT_EQ(*snapshot.max, 35.0);
    ASSERT_EQ(snapshot.buckets.size(), 4u);
    ASSERT_EQ(snapshot.buckets[0], 1u);
    ASSERT_EQ(snapshot.buckets[1], 1u);
    ASSERT_EQ(snapshot.buckets[2], 1u);
    ASSERT_EQ(snapshot.buckets[3], 1u);
}

// Both histogram variants use inclusive lower bounds, so the same boundaries bucket a value
// identically: bucket 0 is (-inf, 10), bucket 1 is [10, 20), bucket 2 is [20, 30), and bucket 3 is
// [30, +inf). A value exactly on a boundary lands in the upper bucket.
TEST(MetricsPrimitivesTest, HistogramBucketsOnInclusiveLowerBounds) {
    Histogram<std::int64_t> histogram({10.0, 20.0, 30.0});
    histogram.observe(10);
    histogram.observe(20);
    histogram.observe(30);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.buckets[0], 0u);
    ASSERT_EQ(snapshot.buckets[1], 1u);  // 10
    ASSERT_EQ(snapshot.buckets[2], 1u);  // 20
    ASSERT_EQ(snapshot.buckets[3], 1u);  // 30
}

TEST(MetricsPrimitivesTest, HistogramVariantsBucketIdentically) {
    const std::vector<double> boundaries = {10.0, 20.0, 30.0};
    const std::vector<std::int64_t> observations = {0, 9, 10, 11, 20, 29, 30, 1000};

    AtomicHistogram<std::int64_t> atomic(boundaries);
    Histogram<std::int64_t> mutexGuarded(boundaries);
    for (const auto value : observations) {
        atomic.observe(value);
        mutexGuarded.observe(value);
    }

    const auto atomicSnapshot = atomic.snapshot();
    const auto mutexSnapshot = mutexGuarded.snapshot();
    ASSERT_EQ(atomicSnapshot.count, mutexSnapshot.count);
    ASSERT_EQ(atomicSnapshot.buckets.size(), mutexSnapshot.buckets.size());
    for (std::size_t i = 0; i < atomicSnapshot.buckets.size(); ++i) {
        ASSERT_EQ(atomicSnapshot.buckets[i], mutexSnapshot.buckets[i]) << "bucket " << i;
    }
}

TEST(MetricsPrimitivesTest, HistogramTracksMinMaxWithNegativeAndFractionalValues) {
    Histogram<double> histogram({0.0, 10.0});
    histogram.observe(-5.5);
    histogram.observe(3.25);
    histogram.observe(-1.0);
    histogram.observe(7.75);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 4u);
    ASSERT_EQ(snapshot.sum, 4.5);
    ASSERT_EQ(*snapshot.min, -5.5);
    ASSERT_EQ(*snapshot.max, 7.75);
    ASSERT_EQ(snapshot.buckets.size(), 3u);
    ASSERT_EQ(snapshot.buckets[0], 2u);  // -5.5, -1.0
    ASSERT_EQ(snapshot.buckets[1], 2u);  // 3.25, 7.75
    ASSERT_EQ(snapshot.buckets[2], 0u);
}

TEST(MetricsPrimitivesTest, HistogramEmptySnapshot) {
    Histogram<std::int64_t> histogram({10.0});
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 0u);
    ASSERT_EQ(snapshot.sum, 0.0);
    ASSERT_FALSE(snapshot.min.has_value());
    ASSERT_FALSE(snapshot.max.has_value());
    ASSERT_EQ(snapshot.buckets.size(), 2u);
}

TEST(MetricsPrimitivesTest, HistogramTracksExactIntegralExtremaAbove2Pow53) {
    // 2^53 + 1 is not representable as a double: storing extrema as doubles would round it to
    // 2^53. Keeping them in the observation type reports it exactly.
    constexpr std::int64_t kValue = (std::int64_t{1} << 53) + 1;
    Histogram<std::int64_t> histogram({});
    histogram.observe(kValue);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 1u);
    ASSERT_TRUE(snapshot.min.has_value());
    ASSERT_TRUE(snapshot.max.has_value());
    ASSERT_EQ(*snapshot.min, kValue);
    ASSERT_EQ(*snapshot.max, kValue);
    // The sum stays a double, so it cannot represent the observation exactly.
    ASSERT_EQ(snapshot.sum, static_cast<double>(std::int64_t{1} << 53));
}

TEST(MetricsPrimitivesTest, HistogramTracksExactIntegralExtremaWithNegativeAndPositive) {
    constexpr std::int64_t kMin = -((std::int64_t{1} << 53) + 1);
    constexpr std::int64_t kMax = (std::int64_t{1} << 53) + 3;
    Histogram<std::int64_t> histogram({});
    histogram.observe(0);
    histogram.observe(kMax);
    histogram.observe(kMin);
    histogram.observe(1);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 4u);
    ASSERT_EQ(*snapshot.min, kMin);
    ASSERT_EQ(*snapshot.max, kMax);
}

TEST(MetricsPrimitivesTest, HistogramConcurrentObserveIsExact) {
    Histogram<double> histogram({10.0});
    constexpr int kThreads = 2;
    constexpr int kIters = 100000;
    std::vector<stdx::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&histogram] {
            for (int i = 0; i < kIters; ++i) {
                histogram.observe(1.0);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    // The mutex serializes observe(), so the totals are exact.
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, std::uint64_t{kThreads} * kIters);
    ASSERT_EQ(snapshot.sum, static_cast<double>(kThreads) * kIters);
    ASSERT_EQ(*snapshot.min, 1.0);
    ASSERT_EQ(*snapshot.max, 1.0);
    ASSERT_EQ(snapshot.buckets[0], std::uint64_t{kThreads} * kIters);
}

TEST(MetricsPrimitivesTest, OptionalMetricWrittenSemanticsPerPrimitiveKind) {
    // isWritten() is false until a recording call arrives, and each kind's recording call sets it
    // even when the recorded value is zero.
    OptionalMetric<Counter<std::int64_t>> counter;
    ASSERT_FALSE(counter.isWritten());
    counter.add(0);
    ASSERT_TRUE(counter.isWritten());

    OptionalMetric<UpDownCounter<std::int64_t>> upDown;
    ASSERT_FALSE(upDown.isWritten());
    upDown.add(0);
    ASSERT_TRUE(upDown.isWritten());

    OptionalMetric<Gauge<std::int64_t>> gauge;
    ASSERT_FALSE(gauge.isWritten());
    gauge.set(0);
    ASSERT_TRUE(gauge.isWritten());
    ASSERT_EQ(gauge(), 0);

    OptionalMetric<Gauge<std::int64_t>> gaugePositive;
    ASSERT_FALSE(gaugePositive.isWritten());
    gaugePositive.set(42);
    ASSERT_TRUE(gaugePositive.isWritten());
    ASSERT_EQ(gaugePositive(), 42);

    OptionalMetric<HighWaterMark<std::int64_t>> highWaterMark;
    ASSERT_FALSE(highWaterMark.isWritten());
    highWaterMark.setToMax(0);
    ASSERT_TRUE(highWaterMark.isWritten());

    OptionalMetric<HighWaterMark<std::int64_t>> highWaterMarkReset;
    ASSERT_FALSE(highWaterMarkReset.isWritten());
    highWaterMarkReset.reset(0);
    ASSERT_TRUE(highWaterMarkReset.isWritten());

    OptionalMetric<AtomicHistogram<std::int64_t>> atomicHistogram;
    ASSERT_FALSE(atomicHistogram.isWritten());
    atomicHistogram.observe(0);
    ASSERT_TRUE(atomicHistogram.isWritten());

    OptionalMetric<Histogram<std::int64_t>> histogram;
    ASSERT_FALSE(histogram.isWritten());
    histogram.observe(0);
    ASSERT_TRUE(histogram.isWritten());
}

TEST(MetricsPrimitivesTest, OptionalMetricWrittenIsSticky) {
    // The wrapper owns the written flag, so it stays set once any recording call has occurred.
    OptionalMetric<Counter<std::int64_t>> counter;
    counter.add(3);
    ASSERT_TRUE(counter.isWritten());
    counter.add(0);
    ASSERT_TRUE(counter.isWritten());
    ASSERT_EQ(counter(), 3);
}

TEST(MetricsPrimitivesTest, OptionalMetricGetIsConstOnly) {
    // get() exists for read-only inspection; recording must go through the wrapper's methods so
    // that written-tracking cannot be bypassed. A mutable get() overload would be selected for a
    // non-const wrapper and return a non-const reference, so asserting the reference is const pins
    // the const-only contract: reintroducing a mutable overload fails to compile.
    using Wrapper = OptionalMetric<Counter<std::int64_t>>;
    static_assert(
        std::is_const_v<std::remove_reference_t<decltype(std::declval<Wrapper&>().get())>>);
    // The const reference is still usable for reading.
    Wrapper wrapper;
    wrapper.add(4);
    ASSERT_EQ(wrapper.get()(), 4);
}

TEST(MetricsPrimitivesTest, OptionalMetricWrittenDoesNotRequirePrimitiveIsWritten) {
    // The histograms deliberately have no isWritten(); the wrapper tracks written-ness on its own.
    OptionalMetric<AtomicHistogram<std::int64_t>> atomicHistogram(std::vector<double>{10.0});
    ASSERT_FALSE(atomicHistogram.isWritten());
    atomicHistogram.observe(5);
    ASSERT_TRUE(atomicHistogram.isWritten());

    OptionalMetric<Histogram<std::int64_t>> histogram(std::vector<double>{10.0});
    ASSERT_FALSE(histogram.isWritten());
    histogram.observe(5);
    ASSERT_TRUE(histogram.isWritten());
}

TEST(MetricsPrimitivesTest, OptionalMetricForwardsRecordingAndReadApi) {
    OptionalMetric<Counter<std::int64_t>> counter;
    counter.add(2);
    counter.add(3);
    ASSERT_EQ(counter(), 5);

    OptionalMetric<Gauge<std::int64_t>> gauge;
    ASSERT_FALSE(gauge.isWritten());
    gauge.set(7);
    ASSERT_TRUE(gauge.isWritten());
    ASSERT_EQ(gauge(), 7);

    OptionalMetric<HighWaterMark<std::int64_t>> highWaterMark;
    highWaterMark.setToMax(10);
    highWaterMark.setToMax(4);
    ASSERT_EQ(highWaterMark(), 10);
    highWaterMark.reset(2);
    ASSERT_EQ(highWaterMark(), 2);

    OptionalMetric<TimedHighWaterMark<std::int64_t>> timedHighWaterMark;
    timedHighWaterMark.setToMax(10);
    timedHighWaterMark.setToMax(4);
    ASSERT_EQ(timedHighWaterMark(), 10);
    timedHighWaterMark.reset(2);
    ASSERT_EQ(timedHighWaterMark(), 2);
    timedHighWaterMark.resetAfter(std::chrono::milliseconds{100});
    timedHighWaterMark.setToMax(3);
    ASSERT_EQ(timedHighWaterMark(), 3);

    OptionalMetric<AtomicHistogram<std::int64_t>> atomicHistogram(std::vector<double>{10.0});
    atomicHistogram.observe(5);
    atomicHistogram.observe(50);
    const auto atomicSnapshot = atomicHistogram.snapshot();
    ASSERT_EQ(atomicSnapshot.count, 2u);
    ASSERT_EQ(atomicSnapshot.buckets[0], 1u);
    ASSERT_EQ(atomicSnapshot.buckets[1], 1u);

    OptionalMetric<Histogram<std::int64_t>> histogram(std::vector<double>{10.0});
    histogram.observe(-1);
    histogram.observe(11);
    const auto snapshot = histogram.snapshot();
    ASSERT_EQ(snapshot.count, 2u);
    ASSERT_EQ(*snapshot.min, -1.0);
    ASSERT_EQ(*snapshot.max, 11.0);
    ASSERT_EQ(snapshot.buckets[0], 1u);
    ASSERT_EQ(snapshot.buckets[1], 1u);
}

TEST(MetricsPrimitivesTest, OptionalMetricResetAfterAloneDoesNotMarkWritten) {
    // resetAfter() only reconfigures the window; it records nothing, so it must not flip
    // isWritten(), matching TimedHighWaterMark's own semantics.
    OptionalMetric<TimedHighWaterMark<std::int64_t>> highWaterMark;
    highWaterMark.resetAfter(std::chrono::milliseconds{100});
    ASSERT_FALSE(highWaterMark.isWritten());
}

TEST(MetricsPrimitivesTest, OptionalMetricPublishesRecordedValueBeforeWrittenFlag) {
    // The mark is published after the recording, so a thread that observes isWritten() true must
    // also observe the recorded value.
    OptionalMetric<Gauge<std::int64_t>> gauge;
    stdx::thread writer([&gauge] { gauge.set(7); });
    while (!gauge.isWritten()) {
    }
    ASSERT_EQ(gauge(), 7);
    writer.join();
}

TEST(MetricsPrimitivesTest, CacheExclusiveForwardsPrimitiveApi) {
    mongo::CacheExclusive<Counter<std::int64_t>> counter;
    counter->add(4);
    ASSERT_EQ((*counter)(), 4);
    ASSERT_EQ(counter->operator()(), 4);

    mongo::CacheExclusive<Gauge<std::int64_t>> gauge;
    ASSERT_EQ(gauge->operator()(), 0);
    gauge->set(9);
    ASSERT_EQ(gauge->operator()(), 9);

    mongo::CacheExclusive<AtomicHistogram<std::int64_t>> atomicHistogram(std::vector<double>{10.0});
    atomicHistogram->observe(5);
    const auto snapshot = atomicHistogram->snapshot();
    ASSERT_EQ(snapshot.count, 1u);
    ASSERT_EQ(snapshot.buckets[0], 1u);
}

TEST(MetricsPrimitivesTest, CacheExclusiveIsCacheLineAlignedAndSized) {
    using Wrapped = mongo::CacheExclusive<Counter<std::int64_t>>;
    static_assert(alignof(Wrapped) >= std::hardware_destructive_interference_size);
    static_assert(sizeof(Wrapped) >= std::hardware_destructive_interference_size);

    std::array<Wrapped, 2> metrics;
    const auto* first = reinterpret_cast<const std::byte*>(&metrics[0]);
    const auto* second = reinterpret_cast<const std::byte*>(&metrics[1]);
    ASSERT_GE(second - first,
              static_cast<std::ptrdiff_t>(std::hardware_destructive_interference_size));
    ASSERT_EQ(reinterpret_cast<std::uintptr_t>(&metrics[0]) %
                  std::hardware_destructive_interference_size,
              0u);
}

TEST(MetricsPrimitivesTest, CacheExclusiveComposesWithOptionalMetric) {
    mongo::CacheExclusive<OptionalMetric<Counter<std::int64_t>>> counter;
    ASSERT_FALSE(counter->isWritten());
    counter->add(0);
    ASSERT_TRUE(counter->isWritten());
    ASSERT_EQ(counter->operator()(), 0);
    counter->add(5);
    ASSERT_EQ(counter->operator()(), 5);
}

TEST(MetricsPrimitivesTest, SynchronizedValueStringRoundTrip) {
    SynchronizedValue<std::string> value;
    ASSERT_EQ(value.get(), "");
    ASSERT_EQ(value(), "");
    value.set("first");
    ASSERT_EQ(value.get(), "first");
    ASSERT_EQ(value(), "first");
    value.set("second");
    ASSERT_EQ(value.get(), "second");
    ASSERT_EQ(value(), "second");
}

TEST(MetricsPrimitivesTest, SynchronizedValueBsonObjRoundTrip) {
    SynchronizedValue<BSONObj> value;
    value.set(BSON("a" << 1 << "b" << "two"));
    const auto first = value.get();
    ASSERT_EQ(first["a"].numberInt(), 1);
    ASSERT_EQ(first["b"].String(), "two");

    value.set(BSON("c" << true));
    const auto second = value();
    ASSERT_TRUE(second["c"].Bool());
    // operator() aliases get(), so both read back the same stored value.
    ASSERT_EQ(value.get().toString(), second.toString());
}

TEST(MetricsPrimitivesTest, SynchronizedValueReturnsIndependentCopy) {
    SynchronizedValue<std::string> value;
    value.set("original");
    // Mutating the returned string must not reach into the stored value.
    auto copy = value.get();
    copy[0] = 'X';
    copy.append("-mutated");
    ASSERT_EQ(value.get(), "original");
    ASSERT_EQ(value(), "original");

    SynchronizedValue<BSONObj> obj;
    obj.set(BSON("a" << 1));
    const auto objCopy = obj.get();
    ASSERT_EQ(objCopy.toString(), obj().toString());
}

TEST(MetricsPrimitivesTest, SynchronizedValueConcurrentStringStress) {
    SynchronizedValue<std::string> value;
    constexpr int kWriters = 4;
    constexpr int kReaders = 4;
    constexpr int kIters = 20000;
    constexpr std::size_t kLen = 64;

    // Each writer stores a single distinct string, so any value a reader observes that is not one
    // of these (or a mixture of two of them) is a torn read.
    std::vector<std::string> written;
    written.reserve(kWriters);
    for (int t = 0; t < kWriters; ++t) {
        written.push_back(std::string(kLen, static_cast<char>('A' + t)));
    }
    // Seed a legal value so a reader cannot observe the default-constructed state.
    value.set(written.front());

    Atomic<bool> stop{false};
    std::vector<std::string> violations(kReaders);
    std::vector<stdx::thread> threads;
    threads.reserve(kWriters + kReaders);
    for (int t = 0; t < kWriters; ++t) {
        threads.emplace_back([&value, &written, t] {
            for (int i = 0; i < kIters; ++i) {
                value.set(written[t]);
            }
        });
    }
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&value, &written, &stop, &violations, r] {
            while (!stop.loadRelaxed()) {
                const auto observed = value.get();
                if (std::find(written.begin(), written.end(), observed) == written.end()) {
                    violations[r] = observed;
                    return;
                }
            }
        });
    }
    // Writers occupy the first kWriters slots; join them, then stop the readers.
    for (int t = 0; t < kWriters; ++t) {
        threads[t].join();
    }
    stop.storeRelaxed(true);
    for (int r = kWriters; r < kWriters + kReaders; ++r) {
        threads[r].join();
    }
    for (int r = 0; r < kReaders; ++r) {
        ASSERT_TRUE(violations[r].empty())
            << "reader " << r << " observed a torn value: " << violations[r];
    }
}

TEST(MetricsPrimitivesTest, SynchronizedValueConcurrentBsonObjStress) {
    SynchronizedValue<BSONObj> value;
    constexpr int kWriters = 4;
    constexpr int kReaders = 4;
    constexpr int kIters = 20000;
    constexpr std::size_t kPadLen = 64;

    std::vector<BSONObj> written;
    written.reserve(kWriters);
    for (int t = 0; t < kWriters; ++t) {
        written.push_back(
            BSON("v" << t << "pad" << std::string(kPadLen, static_cast<char>('A' + t))));
    }
    value.set(written.front());

    Atomic<bool> stop{false};
    std::vector<std::string> violations(kReaders);
    std::vector<stdx::thread> threads;
    threads.reserve(kWriters + kReaders);
    for (int t = 0; t < kWriters; ++t) {
        threads.emplace_back([&value, &written, t] {
            for (int i = 0; i < kIters; ++i) {
                value.set(written[t]);
            }
        });
    }
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&value, &written, &stop, &violations, r] {
            while (!stop.loadRelaxed()) {
                const auto observed = value.get();
                const auto v = observed["v"];
                const auto pad = observed["pad"];
                // A torn read would leave the two fields inconsistent or the document corrupt.
                if (v.type() != BSONType::numberInt || pad.type() != BSONType::string) {
                    violations[r] = "corrupt document: " + observed.toString();
                    return;
                }
                const auto index = v.numberInt();
                if (index < 0 || index >= static_cast<int>(written.size()) ||
                    pad.String() != std::string(kPadLen, static_cast<char>('A' + index))) {
                    violations[r] = "inconsistent document: " + observed.toString();
                    return;
                }
            }
        });
    }
    for (int t = 0; t < kWriters; ++t) {
        threads[t].join();
    }
    stop.storeRelaxed(true);
    for (int r = kWriters; r < kWriters + kReaders; ++r) {
        threads[r].join();
    }
    for (int r = 0; r < kReaders; ++r) {
        ASSERT_TRUE(violations[r].empty())
            << "reader " << r << " observed a torn value: " << violations[r];
    }
}

}  // namespace
}  // namespace mongo::observability
