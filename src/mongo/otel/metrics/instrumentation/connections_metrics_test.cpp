// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/connections_metrics.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/transport/asio/asio_session_manager.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class ConnectionsOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    OtelMetricsCapturer _capturer;
    ConnectionsMetrics _metrics;
};

TEST_F(ConnectionsOtelMetricsTest, UpdateSetsGaugesAndCounters) {
    _metrics.update(
        {.current = 10, .available = 990, .totalCreated = 500, .rejected = 3, .active = 7});

    ASSERT_EQ(10, _capturer.readInt64Gauge(MetricNames::kConnectionsCurrent));
    ASSERT_EQ(990, _capturer.readInt64Gauge(MetricNames::kConnectionsAvailable));
    ASSERT_EQ(500, _capturer.readInt64Counter(MetricNames::kConnectionsTotalCreated));
    ASSERT_EQ(3, _capturer.readInt64Counter(MetricNames::kConnectionsRejected));
    ASSERT_EQ(7, _capturer.readInt64Gauge(MetricNames::kConnectionsActive));
}

TEST_F(ConnectionsOtelMetricsTest, GaugesTrackLatestSnapshot) {
    _metrics.update({.current = 50, .available = 950, .active = 20});

    _metrics.update({.current = 5, .available = 995, .active = 3});

    ASSERT_EQ(5, _capturer.readInt64Gauge(MetricNames::kConnectionsCurrent));
    ASSERT_EQ(995, _capturer.readInt64Gauge(MetricNames::kConnectionsAvailable));
    ASSERT_EQ(3, _capturer.readInt64Gauge(MetricNames::kConnectionsActive));
}

TEST_F(ConnectionsOtelMetricsTest, CountersAccumulateDeltas) {
    _metrics.update({.totalCreated = 100, .rejected = 4});

    _metrics.update({.totalCreated = 150, .rejected = 6});

    // Counter accumulates the total delta across both updates: 150 and 6.
    ASSERT_EQ(150, _capturer.readInt64Counter(MetricNames::kConnectionsTotalCreated));
    ASSERT_EQ(6, _capturer.readInt64Counter(MetricNames::kConnectionsRejected));
}

}  // namespace
}  // namespace mongo
