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

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/meter.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#endif  // MONGO_CONFIG_OTEL

namespace mongo::otel::metrics {
namespace {

class MetricsServiceTest : public testing::Test {
public:
    void SetUp() override {
        metricsService = std::make_unique<MetricsService>();
    }

    std::unique_ptr<MetricsService> metricsService;
};

struct MetricCreatorOptions {
    bool inServerStatus = false;
};

/**
 * Type traits for creating different metric types via MetricsService.
 * Each specialization provides a static `create` method that wraps the
 * appropriate MetricsService::create*() method.
 */
template <typename T>
struct MetricCreator;

template <>
struct MetricCreator<Counter<int64_t>> {
    static Counter<int64_t>& create(MetricsService* svc,
                                    MetricName name,
                                    std::string desc,
                                    MetricUnit unit,
                                    MetricCreatorOptions options = {}) {
        return svc->createInt64Counter(
            name, std::move(desc), unit, {.inServerStatus = options.inServerStatus});
    }
};

template <>
struct MetricCreator<Counter<double>> {
    static Counter<double>& create(MetricsService* svc,
                                   MetricName name,
                                   std::string desc,
                                   MetricUnit unit,
                                   MetricCreatorOptions options = {}) {
        return svc->createDoubleCounter(
            name, std::move(desc), unit, {.inServerStatus = options.inServerStatus});
    }
};

template <>
struct MetricCreator<UpDownCounter<int64_t>> {
    static UpDownCounter<int64_t>& create(MetricsService* svc,
                                          MetricName name,
                                          std::string desc,
                                          MetricUnit unit,
                                          MetricCreatorOptions options = {}) {
        return svc->createInt64UpDownCounter(
            name, std::move(desc), unit, {.inServerStatus = options.inServerStatus});
    }
};

template <>
struct MetricCreator<UpDownCounter<double>> {
    static UpDownCounter<double>& create(MetricsService* svc,
                                         MetricName name,
                                         std::string desc,
                                         MetricUnit unit,
                                         MetricCreatorOptions options = {}) {
        return svc->createDoubleUpDownCounter(
            name, std::move(desc), unit, {.inServerStatus = options.inServerStatus});
    }
};

template <>
struct MetricCreator<Gauge<int64_t>> {
    static Gauge<int64_t>& create(MetricsService* svc,
                                  MetricName name,
                                  std::string desc,
                                  MetricUnit unit,
                                  MetricCreatorOptions options = {}) {
        return svc->createInt64Gauge(
            name, std::move(desc), unit, {.inServerStatus = options.inServerStatus});
    }
};

template <>
struct MetricCreator<Gauge<double>> {
    static Gauge<double>& create(MetricsService* svc,
                                 MetricName name,
                                 std::string desc,
                                 MetricUnit unit,
                                 MetricCreatorOptions options = {}) {
        return svc->createDoubleGauge(
            name, std::move(desc), unit, {.inServerStatus = options.inServerStatus});
    }
};

template <>
struct MetricCreator<Histogram<int64_t>> {
    static Histogram<int64_t>& create(MetricsService* svc,
                                      MetricName name,
                                      std::string desc,
                                      MetricUnit unit,
                                      MetricCreatorOptions options = {}) {
        return svc->createInt64Histogram(
            name, std::move(desc), unit, {.inServerStatus = options.inServerStatus});
    }
};

template <>
struct MetricCreator<Histogram<double>> {
    static Histogram<double>& create(MetricsService* svc,
                                     MetricName name,
                                     std::string desc,
                                     MetricUnit unit,
                                     MetricCreatorOptions options = {}) {
        return svc->createDoubleHistogram(
            name, std::move(desc), unit, {.inServerStatus = options.inServerStatus});
    }
};

/**
 * For each scalar metric type T (int64_t vs double within the same instrument family), maps T to
 * the other width so tests can assert ObjectAlreadyExists when the same name is reused.
 */
template <typename T>
struct AlternativeScalarWidthMetricType;

template <>
struct AlternativeScalarWidthMetricType<Counter<int64_t>> {
    using type = Counter<double>;
};
template <>
struct AlternativeScalarWidthMetricType<Counter<double>> {
    using type = Counter<int64_t>;
};
template <>
struct AlternativeScalarWidthMetricType<UpDownCounter<int64_t>> {
    using type = UpDownCounter<double>;
};
template <>
struct AlternativeScalarWidthMetricType<UpDownCounter<double>> {
    using type = UpDownCounter<int64_t>;
};
template <>
struct AlternativeScalarWidthMetricType<Gauge<int64_t>> {
    using type = Gauge<double>;
};
template <>
struct AlternativeScalarWidthMetricType<Gauge<double>> {
    using type = Gauge<int64_t>;
};
template <>
struct AlternativeScalarWidthMetricType<Histogram<int64_t>> {
    using type = Histogram<double>;
};
template <>
struct AlternativeScalarWidthMetricType<Histogram<double>> {
    using type = Histogram<int64_t>;
};

/**
 * Type-parameterized test fixture for testing metric creation behavior
 * that is common across all metric types (Counter, UpDownCounter, Gauge, Histogram).
 */
template <typename T>
class MetricCreationTest : public MetricsServiceTest {};

using testing::ElementsAre;
using testing::ElementsAreArray;
using MetricTypes = testing::Types<Counter<int64_t>,
                                   Counter<double>,
                                   UpDownCounter<int64_t>,
                                   UpDownCounter<double>,
                                   Gauge<int64_t>,
                                   Gauge<double>,
                                   Histogram<int64_t>,
                                   Histogram<double>>;
TYPED_TEST_SUITE(MetricCreationTest, MetricTypes);

TYPED_TEST(MetricCreationTest, SameMetricReturnedOnSameCreate) {
    auto& metric1 = MetricCreator<TypeParam>::create(
        this->metricsService.get(), MetricNames::kTest1, "description", MetricUnit::kSeconds);
    auto& metric2 = MetricCreator<TypeParam>::create(
        this->metricsService.get(), MetricNames::kTest1, "description", MetricUnit::kSeconds);
    // Initialize MetricsService.
    OtelMetricsCapturer metricsCapturer(*this->metricsService);

    auto& metric3 = MetricCreator<TypeParam>::create(
        this->metricsService.get(), MetricNames::kTest1, "description", MetricUnit::kSeconds);
    EXPECT_EQ(&metric1, &metric2);
    EXPECT_EQ(&metric2, &metric3);
}

TYPED_TEST(MetricCreationTest, ExceptionWhenSameNameButDifferentParameters) {
    MetricCreator<TypeParam>::create(
        this->metricsService.get(), MetricNames::kTest1, "description", MetricUnit::kSeconds);
    ASSERT_THROWS_CODE(MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                        MetricNames::kTest1,
                                                        "different_description",
                                                        MetricUnit::kSeconds),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
    ASSERT_THROWS_CODE(
        MetricCreator<TypeParam>::create(
            this->metricsService.get(), MetricNames::kTest1, "description", MetricUnit::kBytes),
        DBException,
        ErrorCodes::ObjectAlreadyExists);

    // Initialize MetricsService.
    OtelMetricsCapturer metricsCapturer(*this->metricsService);

    ASSERT_THROWS_CODE(MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                        MetricNames::kTest1,
                                                        "different_description",
                                                        MetricUnit::kSeconds),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
    ASSERT_THROWS_CODE(
        MetricCreator<TypeParam>::create(
            this->metricsService.get(), MetricNames::kTest1, "description", MetricUnit::kBytes),
        DBException,
        ErrorCodes::ObjectAlreadyExists);

    ASSERT_THROWS_CODE(MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                        MetricNames::kTest1,
                                                        "description",
                                                        MetricUnit::kSeconds,
                                                        {.inServerStatus = true}),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
}

TYPED_TEST(MetricCreationTest, ExceptionWhenSameNameButDifferentType) {
    MetricCreator<TypeParam>::create(
        this->metricsService.get(), MetricNames::kTest1, "description", MetricUnit::kSeconds);
    // Same instrument family but int64_t vs double (or vice versa) must not register under one
    // name.
    using DifferentType = typename AlternativeScalarWidthMetricType<TypeParam>::type;
    ASSERT_THROWS_CODE(
        MetricCreator<DifferentType>::create(
            this->metricsService.get(), MetricNames::kTest1, "description", MetricUnit::kSeconds),
        DBException,
        ErrorCodes::ObjectAlreadyExists);

    // Initialize MetricsService.
    OtelMetricsCapturer metricsCapturer(*this->metricsService);

    ASSERT_THROWS_CODE(
        MetricCreator<DifferentType>::create(
            this->metricsService.get(), MetricNames::kTest1, "description", MetricUnit::kSeconds),
        DBException,
        ErrorCodes::ObjectAlreadyExists);
}

TEST_F(MetricsServiceTest, ExceptionWhenHistogramBoundariesDifferent) {
    metricsService->createInt64Histogram(
        MetricNames::kTest1,
        "description",
        MetricUnit::kSeconds,
        {.explicitBucketBoundaries = std::vector<double>{10, 100}});
    ASSERT_THROWS_CODE(metricsService->createInt64Histogram(
                           MetricNames::kTest1,
                           "description",
                           MetricUnit::kSeconds,
                           {.explicitBucketBoundaries = std::vector<double>{5, 50}}),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
    ASSERT_THROWS_CODE(
        metricsService->createInt64Histogram(MetricNames::kTest1,
                                             "description",
                                             MetricUnit::kSeconds,
                                             {.explicitBucketBoundaries = boost::none}),
        DBException,
        ErrorCodes::ObjectAlreadyExists);

    metricsService->createDoubleHistogram(
        MetricNames::kTest2,
        "description",
        MetricUnit::kSeconds,
        {.explicitBucketBoundaries = std::vector<double>{10, 100}});
    ASSERT_THROWS_CODE(metricsService->createDoubleHistogram(
                           MetricNames::kTest2,
                           "description",
                           MetricUnit::kSeconds,
                           {.explicitBucketBoundaries = std::vector<double>{5, 50}}),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
    ASSERT_THROWS_CODE(
        metricsService->createDoubleHistogram(MetricNames::kTest2,
                                              "description",
                                              MetricUnit::kSeconds,
                                              {.explicitBucketBoundaries = boost::none}),
        DBException,
        ErrorCodes::ObjectAlreadyExists);
}

TEST_F(MetricsServiceTest, CreateCounterBeforeInitialization) {
    auto& int64Counter = metricsService->createInt64Counter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);
    auto& doubleCounter = metricsService->createDoubleCounter(
        MetricNames::kTest2, "description", MetricUnit::kSeconds);

    // Initialize the MetricsService.
    OtelMetricsCapturer metricsCapturer(*metricsService);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 0);
        EXPECT_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest2), 0.0);
    }

    int64Counter.add(5);
    doubleCounter.add(5.0);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 5);
        EXPECT_DOUBLE_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest2), 5.0);
    }
}

TEST_F(MetricsServiceTest, CreateUpDownCounterBeforeInitialization) {
    auto& int64UpDown = metricsService->createInt64UpDownCounter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);
    auto& doubleUpDown = metricsService->createDoubleUpDownCounter(
        MetricNames::kTest2, "description", MetricUnit::kSeconds);

    OtelMetricsCapturer metricsCapturer(*metricsService);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 0);
        EXPECT_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest2), 0.0);
    }

    int64UpDown.add(5);
    doubleUpDown.add(5.0);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 5);
        EXPECT_DOUBLE_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest2), 5.0);
    }
}

TEST_F(MetricsServiceTest, CreateGaugeBeforeInitialization) {
    auto& int64Gauge =
        metricsService->createInt64Gauge(MetricNames::kTest1, "description", MetricUnit::kSeconds);
    auto& doubleGauge =
        metricsService->createDoubleGauge(MetricNames::kTest2, "description", MetricUnit::kSeconds);

    // Initialize the MetricsService.
    OtelMetricsCapturer metricsCapturer(*metricsService);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest1), 0);
        EXPECT_EQ(metricsCapturer.readDoubleGauge(MetricNames::kTest2), 0.0);
    }

    int64Gauge.set(5);
    doubleGauge.set(5.0);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest1), 5);
        EXPECT_DOUBLE_EQ(metricsCapturer.readDoubleGauge(MetricNames::kTest2), 5.0);
    }
}

#ifdef MONGO_CONFIG_OTEL
// Assert that when a valid MeterProvider in place, we create a working Meter implementation with
// the expected metadata.
TEST_F(MetricsServiceTest, MeterIsInitialized) {
    // Set up a valid MeterProvider.
    OtelMetricsCapturer metricsCapturer(*metricsService);

    std::shared_ptr<opentelemetry::metrics::MeterProvider> meterProvider =
        opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(meterProvider);

    auto* meter =
        meterProvider->GetMeter(toStdStringViewForInterop(MetricsService::kMeterName)).get();
    auto* sdkMeter = dynamic_cast<opentelemetry::sdk::metrics::Meter*>(meter);
    ASSERT_TRUE(sdkMeter);

    const auto* scope = sdkMeter->GetInstrumentationScope();
    ASSERT_TRUE(scope);
    EXPECT_EQ(scope->GetName(), std::string{MetricsService::kMeterName});
}

// Assert that we create a NoopMeter if the global MeterProvider hasn't been set.
TEST_F(MetricsServiceTest, NoOpMeterProviderBeforeInit) {
    std::shared_ptr<opentelemetry::metrics::MeterProvider> meterProvider =
        opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT(meterProvider != nullptr);
    EXPECT_TRUE(isNoopMeter(
        meterProvider->GetMeter(toStdStringViewForInterop(MetricsService::kMeterName)).get()));
}
#endif  // MONGO_CONFIG_OTEL

using SerializeMetricsTest = MetricsServiceTest;

TEST_F(SerializeMetricsTest, SerializesMetrics) {
    auto& int64Histogram = metricsService->createInt64Histogram(
        MetricNames::kTest1, "description", MetricUnit::kSeconds, {.inServerStatus = true});
    auto& doubleHistogram = metricsService->createDoubleHistogram(
        MetricNames::kTest2, "description", MetricUnit::kSeconds, {.inServerStatus = true});
    auto& counter = metricsService->createInt64Counter(
        MetricNames::kTest3, "description", MetricUnit::kSeconds, {.inServerStatus = true});
    auto& gauge = metricsService->createDoubleGauge(
        MetricNames::kTest4, "description", MetricUnit::kQueries, {.inServerStatus = true});
    auto& int64UpDown = metricsService->createInt64UpDownCounter(
        MetricNames::kTest5, "description", MetricUnit::kSeconds, {.inServerStatus = true});
    auto& doubleUpDown = metricsService->createDoubleUpDownCounter(
        MetricNames::kTest6, "description", MetricUnit::kSeconds, {.inServerStatus = true});
    int64Histogram.record(10);
    doubleHistogram.record(20);
    counter.add(1);
    gauge.set(0.33);
    int64UpDown.add(4);
    int64UpDown.add(-1);
    doubleUpDown.add(2.0);
    doubleUpDown.add(-0.5);

    BSONObjBuilder expectedBson;
    expectedBson.append("test_only.metric1_seconds", BSON("average" << 10.0 << "count" << 1));
    expectedBson.append("test_only.metric2_seconds", BSON("average" << 20.0 << "count" << 1));
    expectedBson.append("test_only.metric3_seconds", 1);
    expectedBson.append("test_only.metric4_queries", 0.33);
    expectedBson.append("test_only.metric5_seconds", 3);
    expectedBson.append("test_only.metric6_seconds", 1.5);
    expectedBson.doneFast();

    BSONObjBuilder builder;
    metricsService->appendMetricsForServerStatus(builder);
    ASSERT_BSONOBJ_EQ(builder.obj(), expectedBson.obj());
}

TEST_F(SerializeMetricsTest, ExcludesMetricsNotInServerStatus) {
    auto& int64Histogram = metricsService->createInt64Histogram(
        MetricNames::kTest1, "description", MetricUnit::kSeconds, {.inServerStatus = false});
    // Only doubleHistogram is in the constructed object.
    auto& doubleHistogram = metricsService->createDoubleHistogram(
        MetricNames::kTest2, "description", MetricUnit::kSeconds, {.inServerStatus = true});
    auto& counter = metricsService->createInt64Counter(
        MetricNames::kTest3, "description", MetricUnit::kSeconds, {.inServerStatus = false});
    auto& gauge = metricsService->createDoubleGauge(
        MetricNames::kTest4, "description", MetricUnit::kQueries, {.inServerStatus = false});
    int64Histogram.record(10);
    doubleHistogram.record(20);
    counter.add(1);
    gauge.set(0.33);

    BSONObjBuilder expectedBson;
    expectedBson.append("test_only.metric2_seconds", BSON("average" << 20.0 << "count" << 1));
    expectedBson.doneFast();

    BSONObjBuilder builder;
    metricsService->appendMetricsForServerStatus(builder);
    ASSERT_BSONOBJ_EQ(builder.obj(), expectedBson.obj());
}

using CreateInt64CounterTest = MetricsServiceTest;

TEST_F(CreateInt64CounterTest, RecordsValues) {
    Counter<int64_t>& counter1 = metricsService->createInt64Counter(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);

    OtelMetricsCapturer metricsCapturer(*metricsService);

    Counter<int64_t>& counter2 =
        metricsService->createInt64Counter(MetricNames::kTest2, "description2", MetricUnit::kBytes);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 0);
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest2), 0);
    }

    counter1.add(10);
    counter2.add(1);
    counter1.add(5);
    counter2.add(1);
    counter2.add(1);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 15);
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest2), 3);
    }

    counter1.add(5);
    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 20);
    }
}

using CreateDoubleCounterTest = MetricsServiceTest;

TEST_F(CreateDoubleCounterTest, RecordsValues) {
    Counter<double>& counter1 = metricsService->createDoubleCounter(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);

    OtelMetricsCapturer metricsCapturer(*metricsService);

    Counter<double>& counter2 = metricsService->createDoubleCounter(
        MetricNames::kTest2, "description2", MetricUnit::kBytes);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest1), 0.0);
        EXPECT_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest2), 0.0);
    }

    counter1.add(10.5);
    counter2.add(1.25);
    counter1.add(5.5);
    counter2.add(1.25);
    counter2.add(1.25);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_DOUBLE_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest1), 16.0);
        EXPECT_DOUBLE_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest2), 3.75);
    }
}


using CreateInt64UpDownCounterTest = MetricsServiceTest;

TEST_F(CreateInt64UpDownCounterTest, RecordsValues) {
    OtelMetricsCapturer metricsCapturer(*metricsService);

    UpDownCounter<int64_t>& u1 = metricsService->createInt64UpDownCounter(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);

    UpDownCounter<int64_t>& u2 = metricsService->createInt64UpDownCounter(
        MetricNames::kTest2, "description2", MetricUnit::kBytes);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 0);
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest2), 0);
    }

    u1.add(10);
    u2.add(3);
    u1.add(5);
    u2.add(-1);
    u1.add(-4);
    u2.add(2);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 11);
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest2), 4);
    }

    u1.add(-1);
    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 10);
    }
}

using CreateDoubleUpDownCounterTest = MetricsServiceTest;

TEST_F(CreateDoubleUpDownCounterTest, RecordsValues) {
    OtelMetricsCapturer metricsCapturer(*metricsService);

    UpDownCounter<double>& u1 = metricsService->createDoubleUpDownCounter(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);

    UpDownCounter<double>& u2 = metricsService->createDoubleUpDownCounter(
        MetricNames::kTest2, "description2", MetricUnit::kBytes);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest1), 0.0);
        EXPECT_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest2), 0.0);
    }

    u1.add(10.5);
    u2.add(1.25);
    u1.add(5.5);
    u2.add(-0.5);
    u2.add(1.25);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_DOUBLE_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest1), 16.0);
        EXPECT_DOUBLE_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest2), 2.0);
    }

    u1.add(-0.5);
    if (metricsCapturer.canReadMetrics()) {
        EXPECT_DOUBLE_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest1), 15.5);
    }
}

using CreateInt64GaugeTest = MetricsServiceTest;

TEST_F(CreateInt64GaugeTest, RecordsValues) {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    Gauge<int64_t>& gauge_1 =
        metricsService->createInt64Gauge(MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    Gauge<int64_t>& gauge_2 =
        metricsService->createInt64Gauge(MetricNames::kTest2, "description2", MetricUnit::kBytes);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest1), 0);
        EXPECT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest2), 0);
    }

    gauge_1.set(10);
    gauge_2.set(3);

    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest1), 10);
        EXPECT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest2), 3);
    }

    gauge_1.set(20);
    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Gauge(MetricNames::kTest1), 20);
    }
}

using CreateDoubleGaugeTest = MetricsServiceTest;

TEST_F(CreateDoubleGaugeTest, RecordsValues) {
    Gauge<double>& gauge1 = metricsService->createDoubleGauge(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    OtelMetricsCapturer metricsCapturer(*metricsService);
    Gauge<double>& gauge2 =
        metricsService->createDoubleGauge(MetricNames::kTest2, "description2", MetricUnit::kBytes);

    if (metricsCapturer.canReadMetrics()) {
        ASSERT_EQ(metricsCapturer.readDoubleGauge(MetricNames::kTest1), 0.0);
        ASSERT_EQ(metricsCapturer.readDoubleGauge(MetricNames::kTest2), 0.0);
    }

    gauge1.set(10.5);
    gauge2.set(3.5);

    if (metricsCapturer.canReadMetrics()) {
        ASSERT_EQ(metricsCapturer.readDoubleGauge(MetricNames::kTest1), 10.5);
        ASSERT_EQ(metricsCapturer.readDoubleGauge(MetricNames::kTest2), 3.5);
    }

    gauge1.set(20.8);
    if (metricsCapturer.canReadMetrics()) {
        ASSERT_EQ(metricsCapturer.readDoubleGauge(MetricNames::kTest1), 20.8);
    }
}

using CreateHistogramTest = MetricsServiceTest;

TEST_F(CreateHistogramTest, RecordsInt64Values) {
    auto& histogram1 = metricsService->createInt64Histogram(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);

    // Initialize the MetricsService.
    OtelMetricsCapturer metricsCapturer(*metricsService);

    auto& histogram2 = metricsService->createInt64Histogram(
        MetricNames::kTest2, "description", MetricUnit::kSeconds);

    const std::vector<double> expectedBoundaries = {
        0, 5, 10, 25, 50, 75, 100, 250, 500, 750, 1000, 2500, 5000, 7500, 10000};

    histogram1.record(5);
    if (metricsCapturer.canReadMetrics()) {
        const auto data1 = metricsCapturer.readInt64Histogram(MetricNames::kTest1);
        EXPECT_THAT(data1.boundaries, ElementsAreArray(expectedBoundaries));
        EXPECT_EQ(data1.sum, 5);
        EXPECT_EQ(data1.min, 5);
        EXPECT_EQ(data1.max, 5);
        EXPECT_THAT(data1.counts, ElementsAre(0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
        EXPECT_EQ(data1.count, 1);
    }

    histogram2.record(5);
    if (metricsCapturer.canReadMetrics()) {
        const auto data2 = metricsCapturer.readInt64Histogram(MetricNames::kTest2);
        EXPECT_THAT(data2.boundaries, ElementsAreArray(expectedBoundaries));
        EXPECT_EQ(data2.sum, 5);
        EXPECT_EQ(data2.min, 5);
        EXPECT_EQ(data2.max, 5);
        EXPECT_THAT(data2.counts, ElementsAre(0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
        EXPECT_EQ(data2.count, 1);
    }
}

TEST_F(CreateHistogramTest, RecordsDoubleValues) {
    auto& histogram1 = metricsService->createDoubleHistogram(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);

    // Initialize the MetricsService.
    OtelMetricsCapturer metricsCapturer(*metricsService);

    auto& histogram2 = metricsService->createDoubleHistogram(
        MetricNames::kTest2, "description", MetricUnit::kSeconds);

    const std::vector<double> expectedBoundaries = {
        0, 5, 10, 25, 50, 75, 100, 250, 500, 750, 1000, 2500, 5000, 7500, 10000};

    histogram1.record(103.14);
    if (metricsCapturer.canReadMetrics()) {
        const auto data1 = metricsCapturer.readDoubleHistogram(MetricNames::kTest1);
        EXPECT_THAT(data1.boundaries, ElementsAreArray(expectedBoundaries));
        EXPECT_DOUBLE_EQ(data1.sum, 103.14);
        EXPECT_DOUBLE_EQ(data1.min, 103.14);
        EXPECT_DOUBLE_EQ(data1.max, 103.14);
        EXPECT_THAT(data1.counts, ElementsAre(0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0));
        EXPECT_EQ(data1.count, 1);
    }

    histogram2.record(103.14);
    if (metricsCapturer.canReadMetrics()) {
        const auto data2 = metricsCapturer.readDoubleHistogram(MetricNames::kTest2);
        EXPECT_THAT(data2.boundaries, ElementsAreArray(expectedBoundaries));
        EXPECT_DOUBLE_EQ(data2.sum, 103.14);
        EXPECT_DOUBLE_EQ(data2.min, 103.14);
        EXPECT_DOUBLE_EQ(data2.max, 103.14);
        EXPECT_THAT(data2.counts, ElementsAre(0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0));
        EXPECT_EQ(data2.count, 1);
    }
}

TEST_F(CreateHistogramTest, RecordsInt64ValuesExplicitBoundaries) {
    Histogram<int64_t>& histogram1 = metricsService->createInt64Histogram(
        MetricNames::kTest1,
        "description",
        MetricUnit::kSeconds,
        /*options=*/{.explicitBucketBoundaries = std::vector<double>({2, 4})});
    OtelMetricsCapturer metricsCapturer(*metricsService);
    Histogram<int64_t>& histogram2 = metricsService->createInt64Histogram(
        MetricNames::kTest2,
        "description",
        MetricUnit::kSeconds,
        /*options=*/{.explicitBucketBoundaries = std::vector<double>({10, 100})});

    histogram1.record(15);
    if (metricsCapturer.canReadMetrics()) {
        const auto data1 = metricsCapturer.readInt64Histogram(MetricNames::kTest1);
        EXPECT_THAT(data1.boundaries, ElementsAre(2, 4));
        EXPECT_EQ(data1.sum, 15);
        EXPECT_EQ(data1.min, 15);
        EXPECT_EQ(data1.max, 15);
        EXPECT_THAT(data1.counts, ElementsAre(0, 0, 1));
        EXPECT_EQ(data1.count, 1);
    }

    histogram2.record(2);
    if (metricsCapturer.canReadMetrics()) {
        const auto data2 = metricsCapturer.readInt64Histogram(MetricNames::kTest2);
        EXPECT_THAT(data2.boundaries, ElementsAre(10, 100));
        EXPECT_DOUBLE_EQ(data2.sum, 2);
        EXPECT_DOUBLE_EQ(data2.min, 2);
        EXPECT_DOUBLE_EQ(data2.max, 2);
        EXPECT_THAT(data2.counts, ElementsAre(1, 0, 0));
        EXPECT_EQ(data2.count, 1);
    }
}

TEST_F(CreateHistogramTest, RecordsDoubleValuesExplicitBoundaries) {
    Histogram<double>& histogram1 = metricsService->createDoubleHistogram(
        MetricNames::kTest1,
        "description",
        MetricUnit::kSeconds,
        /*options=*/{.explicitBucketBoundaries = std::vector<double>({2, 4})});
    OtelMetricsCapturer metricsCapturer(*metricsService);
    Histogram<double>& histogram2 = metricsService->createDoubleHistogram(
        MetricNames::kTest2,
        "description",
        MetricUnit::kSeconds,
        /*options=*/{.explicitBucketBoundaries = std::vector<double>({10, 100})});

    histogram1.record(15);
    if (metricsCapturer.canReadMetrics()) {
        const auto data1 = metricsCapturer.readDoubleHistogram(MetricNames::kTest1);
        EXPECT_THAT(data1.boundaries, ElementsAre(2, 4));
        EXPECT_EQ(data1.sum, 15);
        EXPECT_EQ(data1.min, 15);
        EXPECT_EQ(data1.max, 15);
        EXPECT_THAT(data1.counts, ElementsAre(0, 0, 1));
        EXPECT_EQ(data1.count, 1);
    }

    histogram2.record(2);
    if (metricsCapturer.canReadMetrics()) {
        const auto data2 = metricsCapturer.readDoubleHistogram(MetricNames::kTest2);
        EXPECT_THAT(data2.boundaries, ElementsAre(10, 100));
        EXPECT_DOUBLE_EQ(data2.sum, 2);
        EXPECT_DOUBLE_EQ(data2.min, 2);
        EXPECT_DOUBLE_EQ(data2.max, 2);
        EXPECT_THAT(data2.counts, ElementsAre(1, 0, 0));
        EXPECT_EQ(data2.count, 1);
    }
}
}  // namespace
}  // namespace mongo::otel::metrics
