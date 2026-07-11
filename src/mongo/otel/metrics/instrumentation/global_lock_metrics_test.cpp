// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/global_lock_metrics.h"

#include "mongo/db/stats/global_lock_stats.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class GlobalLockOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    OtelMetricsCapturer _capturer;
    GlobalLockMetrics _metrics;
};

TEST_F(GlobalLockOtelMetricsTest, UpdateSetsAllGauges) {
    GlobalLockStatsSnapshot snap;
    snap.totalTimeMicros = 1234;
    snap.activeReaders = 2;
    snap.activeWriters = 3;
    snap.queuedReaders = 4;
    snap.queuedWriters = 5;

    _metrics.update(snap);

    ASSERT_EQ(1234, _capturer.readInt64Gauge(MetricNames::kGlobalLockTotalTime));
    ASSERT_EQ(9, _capturer.readInt64Gauge(MetricNames::kGlobalLockCurrentQueueTotal));
    ASSERT_EQ(4, _capturer.readInt64Gauge(MetricNames::kGlobalLockCurrentQueueReaders));
    ASSERT_EQ(5, _capturer.readInt64Gauge(MetricNames::kGlobalLockCurrentQueueWriters));
    ASSERT_EQ(5, _capturer.readInt64Gauge(MetricNames::kGlobalLockActiveClientsTotal));
    ASSERT_EQ(2, _capturer.readInt64Gauge(MetricNames::kGlobalLockActiveClientsReaders));
    ASSERT_EQ(3, _capturer.readInt64Gauge(MetricNames::kGlobalLockActiveClientsWriters));
}

TEST_F(GlobalLockOtelMetricsTest, GaugesTrackLatestSnapshot) {
    GlobalLockStatsSnapshot first;
    first.totalTimeMicros = 100;
    first.activeReaders = 10;
    first.activeWriters = 20;
    first.queuedReaders = 30;
    first.queuedWriters = 40;
    _metrics.update(first);

    GlobalLockStatsSnapshot later;
    later.totalTimeMicros = 200;
    later.activeReaders = 1;
    later.activeWriters = 2;
    later.queuedReaders = 0;
    later.queuedWriters = 0;
    _metrics.update(later);

    ASSERT_EQ(200, _capturer.readInt64Gauge(MetricNames::kGlobalLockTotalTime));
    ASSERT_EQ(0, _capturer.readInt64Gauge(MetricNames::kGlobalLockCurrentQueueTotal));
    ASSERT_EQ(0, _capturer.readInt64Gauge(MetricNames::kGlobalLockCurrentQueueReaders));
    ASSERT_EQ(0, _capturer.readInt64Gauge(MetricNames::kGlobalLockCurrentQueueWriters));
    ASSERT_EQ(3, _capturer.readInt64Gauge(MetricNames::kGlobalLockActiveClientsTotal));
    ASSERT_EQ(1, _capturer.readInt64Gauge(MetricNames::kGlobalLockActiveClientsReaders));
    ASSERT_EQ(2, _capturer.readInt64Gauge(MetricNames::kGlobalLockActiveClientsWriters));
}

}  // namespace
}  // namespace mongo
