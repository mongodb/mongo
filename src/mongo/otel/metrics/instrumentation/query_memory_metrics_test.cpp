// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/query_memory_metrics.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <limits>

namespace mongo {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class QueryMemoryOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    OtelMetricsCapturer _capturer;
    QueryMemoryMetrics _metrics;
};

TEST_F(QueryMemoryOtelMetricsTest, UpdateSetsGauge) {
    _metrics.update(1024 * 1024);

    ASSERT_EQ(
        1024 * 1024,
        _capturer.readInt64Gauge(MetricNames::kQueryConfiguredMaxMemoryUsageBytesPerOperation));
}

TEST_F(QueryMemoryOtelMetricsTest, GaugeTracksLatestValue) {
    _metrics.update(5000);
    _metrics.update(9000);

    ASSERT_EQ(
        9000,
        _capturer.readInt64Gauge(MetricNames::kQueryConfiguredMaxMemoryUsageBytesPerOperation));
}

TEST_F(QueryMemoryOtelMetricsTest, GaugeReportsUnboundedDefault) {
    // The server parameter defaults to std::numeric_limits<long long>::max(), which represents an
    // effectively unbounded operation-wide memory limit.
    _metrics.update(std::numeric_limits<int64_t>::max());

    ASSERT_EQ(
        std::numeric_limits<int64_t>::max(),
        _capturer.readInt64Gauge(MetricNames::kQueryConfiguredMaxMemoryUsageBytesPerOperation));
}

}  // namespace
}  // namespace mongo
