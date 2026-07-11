// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/index_bulk_builder_metrics.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

// Builds a snapshot with the six emitted fields populated; all other fields default to 0.
IndexBulkBuilderMetricsSnapshot makeSnapshot(long long numSorted,
                                             long long bytesSorted,
                                             long long bytesSpilled,
                                             long long bytesSpilledUncompressed,
                                             long long memUsage,
                                             long long spilledRanges) {
    IndexBulkBuilderMetricsSnapshot s{};
    s.numSorted = numSorted;
    s.bytesSorted = bytesSorted;
    s.bytesSpilled = bytesSpilled;
    s.bytesSpilledUncompressed = bytesSpilledUncompressed;
    s.memUsage = memUsage;
    s.spilledRanges = spilledRanges;
    return s;
}

class IndexBulkBuilderOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    // Must be constructed before any metric is recorded so it resets the process-global
    // instruments and captures subsequent writes.
    OtelMetricsCapturer _capturer;
};

// Updating from a zero baseline (the first-tick scenario) reports the full snapshot value for
// every instrument.
TEST_F(IndexBulkBuilderOtelMetricsTest, FirstTickEmitsFullSnapshot) {
    updateIndexBulkBuilderOtelMetrics(/*prev=*/{}, makeSnapshot(10, 100, 7, 9, 500, 3));

    ASSERT_EQ(10, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderNumSorted));
    ASSERT_EQ(100, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSorted));
    ASSERT_EQ(7, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSpilled));
    ASSERT_EQ(9,
              _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSpilledUncompressed));
    ASSERT_EQ(500, _capturer.readInt64Gauge(MetricNames::kIndexBulkBuilderMemUsage));
    ASSERT_EQ(3, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderSpilledRanges));
}

// Across consecutive ticks, monotonic counters accumulate only the per-tick delta, so their
// running total equals the latest absolute value. The gauge reports the latest snapshot.
TEST_F(IndexBulkBuilderOtelMetricsTest, CountersAccumulateDeltas) {
    auto first = makeSnapshot(10, 100, 7, 9, 500, 3);
    updateIndexBulkBuilderOtelMetrics(/*prev=*/{}, first);

    auto second = makeSnapshot(25, 250, 7, 20, 800, 5);
    updateIndexBulkBuilderOtelMetrics(first, second);

    // 10 + (25-10) == 25, etc. bytesSpilled delta is 0 on the second tick.
    ASSERT_EQ(25, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderNumSorted));
    ASSERT_EQ(250, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSorted));
    ASSERT_EQ(7, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSpilled));
    ASSERT_EQ(20,
              _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSpilledUncompressed));
    // Gauge reflects the latest absolute value.
    ASSERT_EQ(800, _capturer.readInt64Gauge(MetricNames::kIndexBulkBuilderMemUsage));
    // UpDownCounter running total tracks the latest spilledRanges count.
    ASSERT_EQ(5, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderSpilledRanges));
}

// A decreasing snapshot (e.g. after a counter reset) must not push the monotonic counters
// backward: safeDelta() clamps the negative delta to 0. The gauge follows the latest value, and
// the UpDownCounter is allowed to decrease.
TEST_F(IndexBulkBuilderOtelMetricsTest, MonotonicCountersClampOnDecrease) {
    auto first = makeSnapshot(50, 500, 30, 40, 200, 4);
    updateIndexBulkBuilderOtelMetrics(/*prev=*/{}, first);

    auto lower = makeSnapshot(10, 100, 5, 5, 60, 1);
    updateIndexBulkBuilderOtelMetrics(first, lower);

    // Monotonic counters stay at their prior totals (delta clamped to 0).
    ASSERT_EQ(50, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderNumSorted));
    ASSERT_EQ(500, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSorted));
    ASSERT_EQ(30, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSpilled));
    ASSERT_EQ(40,
              _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSpilledUncompressed));
    // Gauge follows the latest (lower) value.
    ASSERT_EQ(60, _capturer.readInt64Gauge(MetricNames::kIndexBulkBuilderMemUsage));
    // UpDownCounter may decrease: 4 + (1-4) == 1.
    ASSERT_EQ(1, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderSpilledRanges));
}

// snapshot() reads the live tracker atomics. Use before/after deltas so the test is robust to any
// accumulated state in the process-global tracker.
TEST(IndexBulkBuilderMetricsSnapshotTest, SnapshotReflectsTrackerWrites) {
    auto& metrics = indexBulkBuilderMetrics();
    const auto before = metrics.snapshot();

    metrics.count.fetchAndAdd(1);
    metrics.resumed.fetchAndAdd(1);
    metrics.sorterTracker.numSorted.fetchAndAdd(7);
    metrics.sorterTracker.bytesSorted.fetchAndAdd(70);
    metrics.sorterTracker.memUsage.fetchAndAdd(13);

    const auto after = metrics.snapshot();
    ASSERT_EQ(before.count + 1, after.count);
    ASSERT_EQ(before.resumed + 1, after.resumed);
    ASSERT_EQ(before.numSorted + 7, after.numSorted);
    ASSERT_EQ(before.bytesSorted + 70, after.bytesSorted);
    ASSERT_EQ(before.memUsage + 13, after.memUsage);
}

}  // namespace
}  // namespace mongo
