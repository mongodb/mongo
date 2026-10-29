// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/connections_metrics.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/transport/asio/asio_session_manager.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;
using namespace std::literals;

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

    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsCurrent), 10);
    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsAvailable), 990);
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsTotalCreated), 500);
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsRejected), 3);
    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsActive), 7);
}

TEST_F(ConnectionsOtelMetricsTest, GaugesTrackLatestSnapshot) {
    _metrics.update({.current = 50, .available = 950, .active = 20});

    _metrics.update({.current = 5, .available = 995, .active = 3});

    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsCurrent), 5);
    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsAvailable), 995);
    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsActive), 3);
}

TEST_F(ConnectionsOtelMetricsTest, CountersAccumulateDeltas) {
    _metrics.update({.totalCreated = 100, .rejected = 4});

    _metrics.update({.totalCreated = 150, .rejected = 6});

    // Counter accumulates the total delta across both updates: 150 and 6.
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsTotalCreated), 150);
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsRejected), 6);
}

TEST_F(ConnectionsOtelMetricsTest, BackpressureUpdateSetsGaugeAndCounterPerVersion) {
    BackpressureConnectionMetrics metrics;
    metrics.increment(0);
    metrics.increment(0);
    metrics.increment(1);

    _metrics.updateBackpressureVersionMetrics(metrics);

    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsBackpressureVersionsCurrent,
                                       std::tuple{kNoBackpressureVersionLabel}),
              2);
    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsBackpressureVersionsCurrent,
                                       std::tuple{"1"sv}),
              1);
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsBackpressureVersionsTotal,
                                         std::tuple{kNoBackpressureVersionLabel}),
              2);
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsBackpressureVersionsTotal,
                                         std::tuple{"1"sv}),
              1);
}

TEST_F(ConnectionsOtelMetricsTest, BackpressureGaugeTracksLatestSnapshot) {
    BackpressureConnectionMetrics metrics;
    metrics.increment(1);
    metrics.increment(1);
    _metrics.updateBackpressureVersionMetrics(metrics);

    metrics.decrement(1);
    _metrics.updateBackpressureVersionMetrics(metrics);

    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsBackpressureVersionsCurrent,
                                       std::tuple{"1"sv}),
              1);
}

TEST_F(ConnectionsOtelMetricsTest, BackpressureCounterAccumulatesDeltas) {
    BackpressureConnectionMetrics metrics;
    metrics.increment(2);
    metrics.increment(2);
    _metrics.updateBackpressureVersionMetrics(metrics);

    metrics.increment(2);
    metrics.increment(2);
    metrics.increment(2);
    _metrics.updateBackpressureVersionMetrics(metrics);

    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsBackpressureVersionsTotal,
                                         std::tuple{"2"sv}),
              5);
}

TEST_F(ConnectionsOtelMetricsTest, BackpressureVersionsGreaterThanMaxUseOtherBucket) {
    BackpressureConnectionMetrics metrics;
    metrics.increment(kMaxExplicitBackpressureVersion + 1);
    metrics.increment(kMaxExplicitBackpressureVersion + 2);
    metrics.increment(kMaxExplicitBackpressureVersion + 2);
    _metrics.updateBackpressureVersionMetrics(metrics);

    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsBackpressureVersionsCurrent,
                                       std::tuple{kBackpressureOtherVersionLabel}),
              3);
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsBackpressureVersionsTotal,
                                         std::tuple{kBackpressureOtherVersionLabel}),
              3);
}

TEST_F(ConnectionsOtelMetricsTest, BackpressureCounterIgnoresNegativeDelta) {
    BackpressureConnectionMetrics higher;
    higher.increment(0);
    higher.increment(0);
    higher.increment(kMaxExplicitBackpressureVersion + 1);
    higher.increment(kMaxExplicitBackpressureVersion + 2);
    _metrics.updateBackpressureVersionMetrics(higher);

    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsBackpressureVersionsTotal,
                                         std::tuple{kNoBackpressureVersionLabel}),
              2);
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsBackpressureVersionsTotal,
                                         std::tuple{kBackpressureOtherVersionLabel}),
              2);

    BackpressureConnectionMetrics lower;
    lower.increment(0);
    lower.increment(kMaxExplicitBackpressureVersion + 1);
    _metrics.updateBackpressureVersionMetrics(lower);

    // A lower totalCreated snapshot yields a negative delta; counter must not decrease.
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsBackpressureVersionsTotal,
                                         std::tuple{kNoBackpressureVersionLabel}),
              2);
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsBackpressureVersionsTotal,
                                         std::tuple{kBackpressureOtherVersionLabel}),
              2);

    // Gauges still track the latest snapshot.
    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsBackpressureVersionsCurrent,
                                       std::tuple{kNoBackpressureVersionLabel}),
              1);
    ASSERT_EQ(_capturer.readInt64Gauge(MetricNames::kConnectionsBackpressureVersionsCurrent,
                                       std::tuple{kBackpressureOtherVersionLabel}),
              1);

    // Recovering to the prior high-water mark must not re-emit already counted creations.
    _metrics.updateBackpressureVersionMetrics(higher);
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsBackpressureVersionsTotal,
                                         std::tuple{kNoBackpressureVersionLabel}),
              2);
    ASSERT_EQ(_capturer.readInt64Counter(MetricNames::kConnectionsBackpressureVersionsTotal,
                                         std::tuple{kBackpressureOtherVersionLabel}),
              2);
}

}  // namespace
}  // namespace mongo
