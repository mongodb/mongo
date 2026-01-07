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

/**
 * Type traits for creating different metric types via MetricsService.
 * Each specialization provides a static `create` method that wraps the
 * appropriate MetricsService::create*() method.
 */
template <typename T>
struct MetricCreator;

template <>
struct MetricCreator<Counter<int64_t>> {
    static Counter<int64_t>* create(MetricsService& svc,
                                    MetricName name,
                                    std::string desc,
                                    MetricUnit unit) {
        return svc.createInt64Counter(name, std::move(desc), unit);
    }
};

template <>
struct MetricCreator<Counter<double>> {
    static Counter<double>* create(MetricsService& svc,
                                   MetricName name,
                                   std::string desc,
                                   MetricUnit unit) {
        return svc.createDoubleCounter(name, std::move(desc), unit);
    }
};

template <>
struct MetricCreator<Gauge<int64_t>> {
    static Gauge<int64_t>* create(MetricsService& svc,
                                  MetricName name,
                                  std::string desc,
                                  MetricUnit unit) {
        return svc.createInt64Gauge(name, std::move(desc), unit);
    }
};

template <>
struct MetricCreator<Histogram<int64_t>> {
    static Histogram<int64_t>* create(MetricsService& svc,
                                      MetricName name,
                                      std::string desc,
                                      MetricUnit unit) {
        return svc.createInt64Histogram(name, std::move(desc), unit);
    }
};

template <>
struct MetricCreator<Histogram<double>> {
    static Histogram<double>* create(MetricsService& svc,
                                     MetricName name,
                                     std::string desc,
                                     MetricUnit unit) {
        return svc.createDoubleHistogram(name, std::move(desc), unit);
    }
};

/**
 * Type-parameterized test fixture for testing metric creation behavior
 * that is common across all metric types (Counter, Gauge, Histogram).
 */
template <typename T>
class MetricCreationTest : public ServiceContextTest {};

using MetricTypes = testing::
    Types<Counter<int64_t>, Counter<double>, Gauge<int64_t>, Histogram<int64_t>, Histogram<double>>;
TYPED_TEST_SUITE(MetricCreationTest, MetricTypes);

TYPED_TEST(MetricCreationTest, SameMetricReturnedOnSameCreate) {
    auto& metricsService = MetricsService::get(this->getServiceContext());
    auto* metric1 = MetricCreator<TypeParam>::create(
        metricsService, MetricNames::kTest1, "description", MetricUnit::kSeconds);
    auto* metric2 = MetricCreator<TypeParam>::create(
        metricsService, MetricNames::kTest1, "description", MetricUnit::kSeconds);
    ASSERT_EQ(metric1, metric2);
}

TYPED_TEST(MetricCreationTest, ExceptionWhenSameNameButDifferentParameters) {
    auto& metricsService = MetricsService::get(this->getServiceContext());
    MetricCreator<TypeParam>::create(
        metricsService, MetricNames::kTest1, "description", MetricUnit::kSeconds);
    ASSERT_THROWS_CODE(
        MetricCreator<TypeParam>::create(
            metricsService, MetricNames::kTest1, "different_description", MetricUnit::kSeconds),
        DBException,
        ErrorCodes::ObjectAlreadyExists);
    ASSERT_THROWS_CODE(MetricCreator<TypeParam>::create(
                           metricsService, MetricNames::kTest1, "description", MetricUnit::kBytes),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
}

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

    BSONObjBuilder expectedBson;
    BSONObjBuilder expectedOtelMetrics(expectedBson.subobjStart("otelMetrics"));
    expectedOtelMetrics.append("test_only.metric1_seconds",
                               BSON("average" << 10.0 << "count" << 1));
    expectedOtelMetrics.append("test_only.metric2_seconds",
                               BSON("average" << 20.0 << "count" << 1));
    expectedOtelMetrics.append("test_only.metric3_seconds", 1);
    expectedOtelMetrics.doneFast();

    ASSERT_BSONOBJ_EQ(metricsService.serializeMetrics(), expectedBson.obj());
}

using CreateInt64CounterTest = MetricsServiceTest;

TEST_F(CreateInt64CounterTest, RecordsValues) {
    OtelMetricsCapturer metricsCapturer;
    auto& metricsService = MetricsService::get(getServiceContext());
    Counter<int64_t>* counter_1 = metricsService.createInt64Counter(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    Counter<int64_t>* counter_2 =
        metricsService.createInt64Counter(MetricNames::kTest2, "description2", MetricUnit::kBytes);

    ASSERT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 0);
    ASSERT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest2), 0);

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

using CreateDoubleCounterTest = MetricsServiceTest;

TEST_F(CreateDoubleCounterTest, RecordsValues) {
    OtelMetricsCapturer metricsCapturer;
    auto& metricsService = MetricsService::get(getServiceContext());
    Counter<double>* counter_1 = metricsService.createDoubleCounter(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    Counter<double>* counter_2 =
        metricsService.createDoubleCounter(MetricNames::kTest2, "description2", MetricUnit::kBytes);

    ASSERT_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest1), 0.0);
    ASSERT_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest2), 0.0);

    counter_1->add(10.5);
    counter_2->add(1.25);
    counter_1->add(5.5);
    counter_2->add(1.25);
    counter_2->add(1.25);

    ASSERT_DOUBLE_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest1), 16.0);
    ASSERT_DOUBLE_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest2), 3.75);
}

using CreateInt64GaugeTest = MetricsServiceTest;

TEST_F(CreateInt64GaugeTest, RecordsValues) {
    OtelMetricsCapturer metricsCapturer;
    auto& metricsService = MetricsService::get(getServiceContext());
    Gauge<int64_t>* gauge_1 =
        metricsService.createInt64Gauge(MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    Gauge<int64_t>* gauge_2 =
        metricsService.createInt64Gauge(MetricNames::kTest2, "description2", MetricUnit::kBytes);

    ASSERT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest1), 0);
    ASSERT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest2), 0);

    gauge_1->set(10);
    gauge_2->set(3);

    ASSERT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest1), 10);
    ASSERT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest2), 3);

    gauge_1->set(20);
    ASSERT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest1), 20);
}

using CreateHistogramTest = MetricsServiceTest;

TEST_F(CreateHistogramTest, RecordsValues) {
    OtelMetricsCapturer metricsCapturer;
    auto& metricsService = MetricsService::get(getServiceContext());
    Histogram<int64_t>* int64Histogram = metricsService.createInt64Histogram(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    Histogram<double>* doubleHistogram = metricsService.createDoubleHistogram(
        MetricNames::kTest2, "description1", MetricUnit::kSeconds);

    const std::vector<double> expectedBoundaries = {
        0, 5, 10, 25, 50, 75, 100, 250, 500, 750, 1000, 2500, 5000, 7500, 10000};

    int64Histogram->record(5);
    const auto data1 = metricsCapturer.readInt64Histogram(MetricNames::kTest1);
    ASSERT_EQ(data1.boundaries, expectedBoundaries);
    ASSERT_EQ(data1.sum, 5);
    ASSERT_EQ(data1.min, 5);
    ASSERT_EQ(data1.max, 5);
    ASSERT_EQ(data1.counts,
              std::vector<uint64_t>({0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    ASSERT_EQ(data1.count, 1);

    int64Histogram->record(15);
    const auto data2 = metricsCapturer.readInt64Histogram(MetricNames::kTest1);
    ASSERT_EQ(data2.boundaries, expectedBoundaries);
    ASSERT_EQ(data2.sum, 20);
    ASSERT_EQ(data2.min, 5);
    ASSERT_EQ(data2.max, 15);
    ASSERT_EQ(data2.counts,
              std::vector<uint64_t>({0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    ASSERT_EQ(data2.count, 2);

    doubleHistogram->record(103.14);
    const auto data3 = metricsCapturer.readDoubleHistogram(MetricNames::kTest2);
    ASSERT_EQ(data3.boundaries, expectedBoundaries);
    ASSERT_EQ(data3.sum, 103.14);
    ASSERT_EQ(data3.min, 103.14);
    ASSERT_EQ(data3.max, 103.14);
    ASSERT_EQ(data3.counts,
              std::vector<uint64_t>({0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}));
    ASSERT_EQ(data3.count, 1);

    doubleHistogram->record(10.0);
    const auto data4 = metricsCapturer.readDoubleHistogram(MetricNames::kTest2);
    ASSERT_EQ(data4.boundaries, expectedBoundaries);
    ASSERT_DOUBLE_EQ(data4.sum, 113.14);
    ASSERT_EQ(data4.min, 10.0);
    ASSERT_EQ(data4.max, 103.14);
    ASSERT_EQ(data4.counts,
              std::vector<uint64_t>({0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}));
    ASSERT_EQ(data4.count, 2);

    // TODO(SERVER-114951): Test custom boundaries and associated counts.
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
