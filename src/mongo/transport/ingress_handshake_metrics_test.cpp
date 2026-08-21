// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/transport/ingress_handshake_metrics.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo::transport {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

TEST(IngressHandshakeMetricsTest, TLSHandshakeLatencyRecordsExactElapsedTime) {
    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    TickSourceMock<Milliseconds> tickSource;
    IngressHandshakeMetrics metrics;

    metrics.onTLSHandshakeStarted(&tickSource);
    tickSource.advance(Milliseconds(123));
    metrics.onTLSHandshakeCompleted();

    const auto histogram = capturer.readInt64Histogram(MetricNames::kIngressTLSHandshakeLatency);
    EXPECT_EQ(histogram.count, 1);
    EXPECT_EQ(histogram.sum, 123);
}

TEST(IngressHandshakeMetricsTest, TLSHandshakeLatencyAccumulatesAcrossSessions) {
    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    TickSourceMock<Milliseconds> tickSource;

    IngressHandshakeMetrics first;
    first.onTLSHandshakeStarted(&tickSource);
    tickSource.advance(Milliseconds(100));
    first.onTLSHandshakeCompleted();

    IngressHandshakeMetrics second;
    second.onTLSHandshakeStarted(&tickSource);
    tickSource.advance(Milliseconds(20));
    second.onTLSHandshakeCompleted();

    const auto histogram = capturer.readInt64Histogram(MetricNames::kIngressTLSHandshakeLatency);
    EXPECT_EQ(histogram.count, 2);
    EXPECT_EQ(histogram.sum, 120);
}

TEST(IngressHandshakeMetricsTest, TLSHandshakeLatencyNotRecordedWithoutHandshakeStart) {
    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    TickSourceMock<Milliseconds> tickSource;

    IngressHandshakeMetrics withHandshake;
    withHandshake.onTLSHandshakeStarted(&tickSource);
    tickSource.advance(Milliseconds(5));
    withHandshake.onTLSHandshakeCompleted();

    // A session that never observed a TLS handshake start must not contribute a sample.
    IngressHandshakeMetrics withoutHandshake;
    withoutHandshake.onTLSHandshakeCompleted();

    const auto histogram = capturer.readInt64Histogram(MetricNames::kIngressTLSHandshakeLatency);
    EXPECT_EQ(histogram.count, 1);
    EXPECT_EQ(histogram.sum, 5);
}

}  // namespace
}  // namespace mongo::transport
