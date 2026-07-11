// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/index_build_metrics.h"

#include "mongo/db/index/index_bulk_builder_metrics.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

IndexBulkBuilderMetricsSnapshot makeSnapshot(long long numSorted,
                                             long long bytesSorted,
                                             long long memUsage,
                                             long long spilledRanges) {
    IndexBulkBuilderMetricsSnapshot s{};
    s.numSorted = numSorted;
    s.bytesSorted = bytesSorted;
    s.memUsage = memUsage;
    s.spilledRanges = spilledRanges;
    return s;
}

class IndexBuildOtelMetricsTest : public unittest::Test {
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

// Regression test for the value-initialized _prevSnapshot: the very first update() must measure
// deltas against a zeroed previous snapshot, so the instruments reflect the full snapshot. An
// uninitialized _prevSnapshot would emit garbage deltas here.
TEST_F(IndexBuildOtelMetricsTest, FirstUpdateMeasuresFromZero) {
    IndexBuildOtelMetrics metrics;
    metrics.update(makeSnapshot(/*numSorted=*/10,
                                /*bytesSorted=*/100,
                                /*memUsage=*/500,
                                /*spilledRanges=*/3));

    ASSERT_EQ(10, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderNumSorted));
    ASSERT_EQ(100, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSorted));
    ASSERT_EQ(500, _capturer.readInt64Gauge(MetricNames::kIndexBulkBuilderMemUsage));
    ASSERT_EQ(3, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderSpilledRanges));
}

// update() retains the previous snapshot between ticks, so monotonic counters accumulate only the
// per-tick delta (running total == latest absolute value) and the gauge reports the latest value.
TEST_F(IndexBuildOtelMetricsTest, SubsequentUpdatesCarryPreviousSnapshot) {
    IndexBuildOtelMetrics metrics;
    metrics.update(makeSnapshot(10, 100, 500, 3));
    metrics.update(makeSnapshot(25, 250, 800, 5));

    ASSERT_EQ(25, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderNumSorted));
    ASSERT_EQ(250, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSorted));
    ASSERT_EQ(800, _capturer.readInt64Gauge(MetricNames::kIndexBulkBuilderMemUsage));
    ASSERT_EQ(5, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderSpilledRanges));
}

}  // namespace
}  // namespace mongo
