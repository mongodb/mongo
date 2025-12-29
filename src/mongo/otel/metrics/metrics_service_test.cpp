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

#include "mongo/otel/metrics/metrics_service.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/meter.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>

namespace mongo::otel::metrics {
namespace {

class MetricsServiceTest : public ServiceContextTest {};

// Assert that when a valid MeterProvider in place, we create a working Meter implementation with
// the expected metadata.
TEST_F(MetricsServiceTest, MeterIsInitialized) {
    // Set up a valid MeterProvider.
    OtelMetricsCapturer metricsCapturer;

    std::shared_ptr<opentelemetry::metrics::MeterProvider> meterProvider =
        opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(meterProvider);

    auto* meter =
        meterProvider->GetMeter(toStdStringViewForInterop(MetricsService::kMeterName)).get();
    auto* sdkMeter = dynamic_cast<opentelemetry::sdk::metrics::Meter*>(meter);
    ASSERT_TRUE(sdkMeter);

    const auto* scope = sdkMeter->GetInstrumentationScope();
    ASSERT_TRUE(scope);
    ASSERT_EQ(scope->GetName(), std::string{MetricsService::kMeterName});
}

// Assert that we create a NoopMeter if the global MeterProvider hasn't been set.
TEST_F(MetricsServiceTest, ServiceContextInitBeforeMeterProvider) {
    std::shared_ptr<opentelemetry::metrics::MeterProvider> meterProvider =
        opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT(meterProvider != nullptr);
    ASSERT_TRUE(isNoopMeter(
        meterProvider->GetMeter(toStdStringViewForInterop(MetricsService::kMeterName)).get()));
}

TEST_F(MetricsServiceTest, SerializeMetrics) {
    auto& metricsService = MetricsService::get(getServiceContext());
    auto int64Histogram = metricsService.createInt64Histogram(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);
    auto doubleHistogram = metricsService.createDoubleHistogram(
        MetricNames::kTest2, "description", MetricUnit::kSeconds);
    auto counter =
        metricsService.createInt64Counter(MetricNames::kTest3, "description", MetricUnit::kSeconds);
    int64Histogram->record(10);
    doubleHistogram->record(20);
    counter->add(1);

    auto expectedInt64HistogramBson =
        BSON("test_only.metric1_seconds" << BSON("average" << 10.0 << "count" << 1));
    auto expectedDoubleHistogramBson =
        BSON("test_only.metric2_seconds" << BSON("average" << 20.0 << "count" << 1));
    // TODO(SERVER-115756): Update expected value.
    auto expectedCounterBson = Document();
    ASSERT_BSONOBJ_EQ(metricsService.serializeMetrics(),
                      BSON("otelMetrics" << expectedInt64HistogramBson << "otelMetrics"
                                         << expectedDoubleHistogramBson << "otelMetrics"
                                         << expectedCounterBson));
}

using CreateInt64CounterTest = MetricsServiceTest;

TEST_F(CreateInt64CounterTest, SameCounterReturnedOnSameCreate) {
    auto& metricsService = MetricsService::get(getServiceContext());
    Counter<int64_t>* counter_1 =
        metricsService.createInt64Counter(MetricNames::kTest1, "description", MetricUnit::kSeconds);
    Counter<int64_t>* counter_2 =
        metricsService.createInt64Counter(MetricNames::kTest1, "description", MetricUnit::kSeconds);
    ASSERT_EQ(counter_1, counter_2);
}

TEST_F(CreateInt64CounterTest, ExceptionWhenSameNameButDifferentParameters) {
    auto& metricsService = MetricsService::get(getServiceContext());
    metricsService.createInt64Counter(MetricNames::kTest1, "description", MetricUnit::kSeconds);
    ASSERT_THROWS_CODE(metricsService.createInt64Counter(
                           MetricNames::kTest1, "different_description", MetricUnit::kSeconds),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
    ASSERT_THROWS_CODE(
        metricsService.createInt64Counter(MetricNames::kTest1, "description", MetricUnit::kBytes),
        DBException,
        ErrorCodes::ObjectAlreadyExists);
}

TEST_F(CreateInt64CounterTest, RecordsValues) {
    OtelMetricsCapturer metricsCapturer;
    auto& metricsService = MetricsService::get(getServiceContext());
    Counter<int64_t>* counter_1 = metricsService.createInt64Counter(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    Counter<int64_t>* counter_2 =
        metricsService.createInt64Counter(MetricNames::kTest2, "description2", MetricUnit::kBytes);

    counter_1->add(10);
    counter_2->add(1);
    counter_1->add(5);
    counter_2->add(1);
    counter_2->add(1);

    ASSERT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 15);
    ASSERT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest2), 3);

    counter_1->add(5);
    ASSERT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 20);
}

using CreateHistogramTest = MetricsServiceTest;

// TODO(SERVER-115964): Add RecordsValue test for histogram.

TEST_F(CreateHistogramTest, SameHistogramReturnedOnSameCreate) {
    auto& metricsService = MetricsService::get(getServiceContext());
    Histogram<int64_t>* histogram_1 = metricsService.createInt64Histogram(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);
    Histogram<int64_t>* histogram_2 = metricsService.createInt64Histogram(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);
    ASSERT_EQ(histogram_1, histogram_2);

    Histogram<double>* histogram_3 = metricsService.createDoubleHistogram(
        MetricNames::kTest2, "description", MetricUnit::kSeconds);
    Histogram<double>* histogram_4 = metricsService.createDoubleHistogram(
        MetricNames::kTest2, "description", MetricUnit::kSeconds);
    ASSERT_EQ(histogram_3, histogram_4);
}

TEST_F(CreateHistogramTest, ExceptionWhenSameNameButDifferentParameters) {
    auto& metricsService = MetricsService::get(getServiceContext());
    metricsService.createInt64Histogram(MetricNames::kTest1, "description", MetricUnit::kSeconds);
    ASSERT_THROWS_CODE(metricsService.createInt64Histogram(
                           MetricNames::kTest1, "different_description", MetricUnit::kSeconds),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
    ASSERT_THROWS_CODE(
        metricsService.createInt64Histogram(MetricNames::kTest1, "description", MetricUnit::kBytes),
        DBException,
        ErrorCodes::ObjectAlreadyExists);

    metricsService.createDoubleHistogram(MetricNames::kTest2, "description", MetricUnit::kSeconds);
    ASSERT_THROWS_CODE(metricsService.createDoubleHistogram(
                           MetricNames::kTest2, "different_description", MetricUnit::kSeconds),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
    ASSERT_THROWS_CODE(metricsService.createDoubleHistogram(
                           MetricNames::kTest2, "description", MetricUnit::kBytes),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
}

TEST_F(CreateHistogramTest, ExceptionWhenSameNameButDifferentType) {
    auto& metricsService = MetricsService::get(getServiceContext());
    metricsService.createInt64Histogram(MetricNames::kTest1, "description", MetricUnit::kSeconds);
    ASSERT_THROWS_CODE(metricsService.createDoubleHistogram(
                           MetricNames::kTest1, "description", MetricUnit::kSeconds),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
}
}  // namespace
}  // namespace mongo::otel::metrics
