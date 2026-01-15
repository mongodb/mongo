/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/otel/metrics/metrics_test_util.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::metrics {

namespace {
class OtelMetricsCapturerTest : public testing::Test {
public:
    void SetUp() override {
        metricsService = std::make_unique<MetricsService>();
    }

    std::unique_ptr<MetricsService> metricsService;
};
}  // namespace

#if MONGO_CONFIG_OTEL
TEST_F(OtelMetricsCapturerTest, ReadThrowsExceptionIfMetricNotFound) {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    ASSERT_THROWS_CODE(metricsCapturer.readInt64Counter(MetricNames::kTest1),
                       DBException,
                       ErrorCodes::KeyNotFound);
    ASSERT_THROWS_CODE(metricsCapturer.readInt64Histogram(MetricNames::kTest1),
                       DBException,
                       ErrorCodes::KeyNotFound);
    ASSERT_THROWS_CODE(metricsCapturer.readDoubleHistogram(MetricNames::kTest1),
                       DBException,
                       ErrorCodes::KeyNotFound);
}

TEST_F(OtelMetricsCapturerTest, HistogramWrongValueTypeThrowsException) {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    Histogram<int64_t>* int64Histogram = metricsService->createInt64Histogram(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    Histogram<double>* doubleHistogram = metricsService->createDoubleHistogram(
        MetricNames::kTest2, "description1", MetricUnit::kSeconds);
    // A value must be recorded for the histogram to be initialized in the underlying metrics
    // exporter.
    int64Histogram->record(1);
    doubleHistogram->record(1);

    ASSERT_THROWS_CODE(metricsCapturer.readDoubleHistogram(MetricNames::kTest1),
                       DBException,
                       ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(metricsCapturer.readInt64Histogram(MetricNames::kTest2),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(OtelMetricsCapturerTest, CounterWrongValueTypeThrowsException) {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    metricsService->createInt64Counter(MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    metricsService->createDoubleCounter(MetricNames::kTest2, "description2", MetricUnit::kSeconds);

    // Reading an int64 counter as a double counter should throw TypeMismatch.
    ASSERT_THROWS_CODE(metricsCapturer.readDoubleCounter(MetricNames::kTest1),
                       DBException,
                       ErrorCodes::TypeMismatch);
    // Reading a double counter as an int64 counter should throw TypeMismatch.
    ASSERT_THROWS_CODE(metricsCapturer.readInt64Counter(MetricNames::kTest2),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(OtelMetricsCapturerTest, GaugeWrongValueTypeThrowsException) {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    metricsService->createInt64Gauge(MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    metricsService->createDoubleGauge(MetricNames::kTest2, "description2", MetricUnit::kSeconds);

    // Reading an int64 gauge as a double gauge should throw TypeMismatch.
    ASSERT_THROWS_CODE(metricsCapturer.readDoubleGauge(MetricNames::kTest1),
                       DBException,
                       ErrorCodes::TypeMismatch);
    // Reading a double gauge as an int64 gauge should throw TypeMismatch.
    ASSERT_THROWS_CODE(
        metricsCapturer.readInt64Gauge(MetricNames::kTest2), DBException, ErrorCodes::TypeMismatch);
}

TEST_F(OtelMetricsCapturerTest, CanReadMetricsIsTrue) {
    EXPECT_TRUE(OtelMetricsCapturer::canReadMetrics());
}

#else

TEST_F(OtelMetricsCapturerTest, CanReadMetricsIsFalse) {
    EXPECT_FALSE(OtelMetricsCapturer::canReadMetrics());
}

using OtelMetricsCapturerDeathTest = OtelMetricsCapturerTest;

DEATH_TEST_F(OtelMetricsCapturerDeathTest, DiesReadingInt64Counter, "doesn't have otel enabled") {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    Counter<int64_t>* int64Counter = metricsService->createInt64Counter(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    int64Counter->add(3);
    metricsCapturer.readInt64Counter(MetricNames::kTest1);
}

DEATH_TEST_F(OtelMetricsCapturerDeathTest, DiesReadingDoubleCounter, "doesn't have otel enabled") {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    Counter<double>* doubleCounter = metricsService->createDoubleCounter(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    doubleCounter->add(3);
    metricsCapturer.readInt64Counter(MetricNames::kTest1);
}

DEATH_TEST_F(OtelMetricsCapturerDeathTest, DiesReadingInt64Gauge, "doesn't have otel enabled") {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    Gauge<int64_t>* int64Gauge =
        metricsService->createInt64Gauge(MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    int64Gauge->set(3);
    metricsCapturer.readInt64Gauge(MetricNames::kTest1);
}

DEATH_TEST_F(OtelMetricsCapturerDeathTest, DiesReadingDoubleGauge, "doesn't have otel enabled") {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    Gauge<double>* doubleGauge = metricsService->createDoubleGauge(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    doubleGauge->set(3);
    metricsCapturer.readDoubleGauge(MetricNames::kTest1);
}

DEATH_TEST_F(OtelMetricsCapturerDeathTest, DiesReadingInt64Histogram, "doesn't have otel enabled") {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    Histogram<int64_t>* int64Histogram = metricsService->createInt64Histogram(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    int64Histogram->record(3);
    metricsCapturer.readInt64Histogram(MetricNames::kTest1);
}

DEATH_TEST_F(OtelMetricsCapturerDeathTest,
             DiesReadingDoubleHistogram,
             "doesn't have otel enabled") {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    Histogram<double>* doubleHistogram = metricsService->createDoubleHistogram(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    doubleHistogram->record(3);
    metricsCapturer.readInt64Histogram(MetricNames::kTest1);
}

#endif  // MONGO_CONFIG_OTEL

// The below tests verify the independence of OtelMetricCapturer instances by sharing the same
// MetricsService among multiple capturers.
TEST_F(OtelMetricsCapturerTest, CreateInt64CounterWithTwoCapturers) {
    auto* metric = metricsService->createInt64Counter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->add(1);
        if (capturer.canReadMetrics()) {
            EXPECT_EQ(capturer.readInt64Counter(MetricNames::kTest1), 1);
        }
    }
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->add(10);
        if (capturer.canReadMetrics()) {
            EXPECT_EQ(capturer.readInt64Counter(MetricNames::kTest1), 10);
        }
    }
}

TEST_F(OtelMetricsCapturerTest, CreateDoubleCounterWithTwoCapturers) {
    auto* metric = metricsService->createDoubleCounter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->add(1.5);
        if (capturer.canReadMetrics()) {
            EXPECT_DOUBLE_EQ(capturer.readDoubleCounter(MetricNames::kTest1), 1.5);
        }
    }
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->add(10.5);
        if (capturer.canReadMetrics()) {
            EXPECT_DOUBLE_EQ(capturer.readDoubleCounter(MetricNames::kTest1), 10.5);
        }
    }
}

TEST_F(OtelMetricsCapturerTest, CreateInt64GaugeWithTwoCapturers) {
    auto* metric =
        metricsService->createInt64Gauge(MetricNames::kTest1, "description", MetricUnit::kSeconds);
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->set(1);
        if (capturer.canReadMetrics()) {
            EXPECT_EQ(capturer.readInt64Gauge(MetricNames::kTest1), 1);
        }
    }
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->set(10);
        if (capturer.canReadMetrics()) {
            EXPECT_EQ(capturer.readInt64Gauge(MetricNames::kTest1), 10);
        }
    }
}

TEST_F(OtelMetricsCapturerTest, CreateDoubleGaugeWithTwoCapturers) {
    auto* metric =
        metricsService->createDoubleGauge(MetricNames::kTest1, "description", MetricUnit::kSeconds);
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->set(1.5);
        if (capturer.canReadMetrics()) {
            EXPECT_EQ(capturer.readDoubleGauge(MetricNames::kTest1), 1.5);
        }
    }
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->set(10.5);
        if (capturer.canReadMetrics()) {
            EXPECT_EQ(capturer.readDoubleGauge(MetricNames::kTest1), 10.5);
        }
    }
}

TEST_F(OtelMetricsCapturerTest, CreateInt64HistogramWithTwoCapturers) {
    auto* metric = metricsService->createInt64Histogram(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->record(1);
        if (capturer.canReadMetrics()) {
            const auto data = capturer.readInt64Histogram(MetricNames::kTest1);
            EXPECT_EQ(data.sum, 1);
            EXPECT_EQ(data.count, 1);
        }
    }
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->record(10);
        if (capturer.canReadMetrics()) {
            const auto data = capturer.readInt64Histogram(MetricNames::kTest1);
            EXPECT_EQ(data.sum, 10);
            EXPECT_EQ(data.count, 1);
        }
    }
}

TEST_F(OtelMetricsCapturerTest, CreateDoubleHistogramWithTwoCapturers) {
    auto* metric = metricsService->createDoubleHistogram(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->record(1.5);
        if (capturer.canReadMetrics()) {
            const auto data = capturer.readDoubleHistogram(MetricNames::kTest1);
            EXPECT_DOUBLE_EQ(data.sum, 1.5);
            EXPECT_EQ(data.count, 1);
        }
    }
    {
        OtelMetricsCapturer capturer(*metricsService);
        metric->record(10.5);
        if (capturer.canReadMetrics()) {
            const auto data = capturer.readDoubleHistogram(MetricNames::kTest1);
            EXPECT_DOUBLE_EQ(data.sum, 10.5);
            EXPECT_EQ(data.count, 1);
        }
    }
}
}  // namespace mongo::otel::metrics
