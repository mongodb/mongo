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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_matcher.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional.hpp>

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

    void TearDown() override {
        metricsService->clearForTests();
    }

    std::unique_ptr<MetricsService> metricsService;
};

/**
 * Maps a concrete instrument type to the `MetricsService::create*` options type.
 */
template <typename InstrumentT>
struct MetricOptionsFor {
    using type = ScalarMetricOptions;
};

template <typename T>
struct MetricOptionsFor<Histogram<T>> {
    using type = HistogramOptions;
};

template <typename InstrumentT>
using MetricOptions = typename MetricOptionsFor<InstrumentT>::type;

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
                                    const MetricOptions<Counter<int64_t>>& options = {}) {
        return svc->createInt64Counter(name, std::move(desc), unit, options);
    }
};

template <>
struct MetricCreator<Counter<double>> {
    static Counter<double>& create(MetricsService* svc,
                                   MetricName name,
                                   std::string desc,
                                   MetricUnit unit,
                                   const MetricOptions<Counter<double>>& options = {}) {
        return svc->createDoubleCounter(name, std::move(desc), unit, options);
    }
};

template <>
struct MetricCreator<UpDownCounter<int64_t>> {
    static UpDownCounter<int64_t>& create(
        MetricsService* svc,
        MetricName name,
        std::string desc,
        MetricUnit unit,
        const MetricOptions<UpDownCounter<int64_t>>& options = {}) {
        return svc->createInt64UpDownCounter(name, std::move(desc), unit, options);
    }
};

template <>
struct MetricCreator<UpDownCounter<double>> {
    static UpDownCounter<double>& create(MetricsService* svc,
                                         MetricName name,
                                         std::string desc,
                                         MetricUnit unit,
                                         const MetricOptions<UpDownCounter<double>>& options = {}) {
        return svc->createDoubleUpDownCounter(name, std::move(desc), unit, options);
    }
};

template <>
struct MetricCreator<Gauge<int64_t>> {
    static Gauge<int64_t>& create(MetricsService* svc,
                                  MetricName name,
                                  std::string desc,
                                  MetricUnit unit,
                                  const MetricOptions<Gauge<int64_t>>& options = {}) {
        return svc->createInt64Gauge(name, std::move(desc), unit, options);
    }
};

template <>
struct MetricCreator<Gauge<double>> {
    static Gauge<double>& create(MetricsService* svc,
                                 MetricName name,
                                 std::string desc,
                                 MetricUnit unit,
                                 const MetricOptions<Gauge<double>>& options = {}) {
        return svc->createDoubleGauge(name, std::move(desc), unit, options);
    }
};

template <>
struct MetricCreator<Histogram<int64_t>> {
    static Histogram<int64_t>& create(MetricsService* svc,
                                      MetricName name,
                                      std::string desc,
                                      MetricUnit unit,
                                      const MetricOptions<Histogram<int64_t>>& options = {}) {
        return svc->createInt64Histogram(name, std::move(desc), unit, options);
    }
};

template <>
struct MetricCreator<Histogram<double>> {
    static Histogram<double>& create(MetricsService* svc,
                                     MetricName name,
                                     std::string desc,
                                     MetricUnit unit,
                                     const MetricOptions<Histogram<double>>& options = {}) {
        return svc->createDoubleHistogram(name, std::move(desc), unit, options);
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

using testing::_;
using testing::AnyOf;
using testing::Contains;
using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::Matcher;
using testing::Not;
using testing::UnorderedElementsAre;
using unittest::match::BSONElementEQ;
using unittest::match::BSONObjElements;
using unittest::match::BSONObjEQ;
using unittest::match::IsBSONElement;
using MetricTypes = testing::Types<Counter<int64_t>,
                                   Counter<double>,
                                   UpDownCounter<int64_t>,
                                   UpDownCounter<double>,
                                   Gauge<int64_t>,
                                   Gauge<double>,
                                   Histogram<int64_t>,
                                   Histogram<double>>;
TYPED_TEST_SUITE(MetricCreationTest, MetricTypes);

TYPED_TEST(MetricCreationTest, CreateRejectsInvalidOtelMetricName) {
    ASSERT_THROWS_CODE(MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                        MetricNames::kTestInvalid,
                                                        "description",
                                                        MetricUnit::kSeconds),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TYPED_TEST(MetricCreationTest, CreateRejectsInvalidServerStatusPath) {
    // Invalid serverStatus segment (snake case).
    MetricOptions<TypeParam> options{.serverStatusOptions = ServerStatusOptions{
                                         .dottedPath = "network.open_connections",
                                         .role = ClusterRole{},
                                     }};
    ASSERT_THROWS_CODE(MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                        MetricNames::kTest1,
                                                        "description",
                                                        MetricUnit::kSeconds,
                                                        options),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

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

TYPED_TEST(MetricCreationTest, SameMetricReturnedWhenCreateWithIdenticalServerStatusOptions) {
    MetricOptions<TypeParam> options{.serverStatusOptions = ServerStatusOptions{
                                         .dottedPath = "network.openConnections",
                                         .role = ClusterRole{},
                                     }};
    auto& m1 = MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                MetricNames::kTest1,
                                                "description",
                                                MetricUnit::kSeconds,
                                                options);
    auto& m2 = MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                MetricNames::kTest1,
                                                "description",
                                                MetricUnit::kSeconds,
                                                options);
    OtelMetricsCapturer metricsCapturer(*this->metricsService);
    auto& m3 = MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                MetricNames::kTest1,
                                                "description",
                                                MetricUnit::kSeconds,
                                                options);
    EXPECT_EQ(&m1, &m2);
    EXPECT_EQ(&m2, &m3);
}

TYPED_TEST(MetricCreationTest, ExceptionWhenInServerStatusAndServerStatusOptionsBothSet) {
    MetricOptions<TypeParam> options{
        .serverStatusOptions = ServerStatusOptions{.dottedPath = "network.openConnections"},
        .inServerStatus = true,
    };
    ASSERT_THROWS_CODE(MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                        MetricNames::kTest1,
                                                        "description",
                                                        MetricUnit::kSeconds,
                                                        options),
                       DBException,
                       12323501);
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

TYPED_TEST(MetricCreationTest, ExceptionWhenSameNameButDifferentServerStatusOptionsNoneVsSet) {
    MetricCreator<TypeParam>::create(
        this->metricsService.get(), MetricNames::kTest1, "description", MetricUnit::kSeconds);

    MetricOptions<TypeParam> options{.serverStatusOptions = ServerStatusOptions{
                                         .dottedPath = "network.openConnections",
                                         .role = ClusterRole{},
                                     }};
    ASSERT_THROWS_CODE(MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                        MetricNames::kTest1,
                                                        "description",
                                                        MetricUnit::kSeconds,
                                                        options),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
}

TYPED_TEST(MetricCreationTest, ExceptionWhenSameNameButDifferentServerStatusOptionsDifferentPaths) {
    MetricOptions<TypeParam> optionsA{
        .serverStatusOptions = ServerStatusOptions{.dottedPath = "network.openConnections"}};
    MetricCreator<TypeParam>::create(this->metricsService.get(),
                                     MetricNames::kTest1,
                                     "description",
                                     MetricUnit::kSeconds,
                                     optionsA);

    MetricOptions<TypeParam> optionsB{
        .serverStatusOptions = ServerStatusOptions{.dottedPath = "ingress.openConnections"}};
    ASSERT_THROWS_CODE(MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                        MetricNames::kTest1,
                                                        "description",
                                                        MetricUnit::kSeconds,
                                                        optionsB),
                       DBException,
                       ErrorCodes::ObjectAlreadyExists);
}

TYPED_TEST(MetricCreationTest, ExceptionWhenSameNameButDifferentServerStatusOptionsDifferentRole) {
    const std::string sharedPath = "network.openConnections";

    MetricOptions<TypeParam> optionsRoleNone{.serverStatusOptions = ServerStatusOptions{
                                                 .dottedPath = sharedPath,
                                                 .role = ClusterRole::None,
                                             }};
    MetricCreator<TypeParam>::create(this->metricsService.get(),
                                     MetricNames::kTest1,
                                     "description",
                                     MetricUnit::kSeconds,
                                     optionsRoleNone);

    MetricOptions<TypeParam> optionsShardRole{.serverStatusOptions = ServerStatusOptions{
                                                  .dottedPath = sharedPath,
                                                  .role = ClusterRole::ShardServer,
                                              }};
    ASSERT_THROWS_CODE(MetricCreator<TypeParam>::create(this->metricsService.get(),
                                                        MetricNames::kTest1,
                                                        "description",
                                                        MetricUnit::kSeconds,
                                                        optionsShardRole),
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

using SerializeMetricsFlatTest = MetricsServiceTest;

TEST_F(SerializeMetricsFlatTest, IncludeMetricsInServerStatus) {
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

TEST_F(SerializeMetricsFlatTest, ExcludesMetricsNotInServerStatus) {
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

using SerializeMetricsTreeTest = MetricsServiceTest;

TEST_F(SerializeMetricsTreeTest, Counter) {
    CounterOptions options{.serverStatusOptions = ServerStatusOptions{
                               .dottedPath = "ingress.openConnections",
                               .role = ClusterRole::None,
                           }};
    auto& counter = metricsService->createInt64Counter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds, options);
    counter.add(42);

    BSONObjBuilder builder;
    mongo::globalMetricTreeSet()[ClusterRole::None].appendTo(builder);
    const BSONObj obj = builder.obj();
    ASSERT_EQ(obj["metrics"]["ingress"]["openConnections"].Long(), 42);
}

TEST_F(SerializeMetricsTreeTest, Histogram) {
    HistogramOptions options{.serverStatusOptions = ServerStatusOptions{
                                 .dottedPath = "ops.latencyHistogram",
                                 .role = ClusterRole::None,
                             }};
    auto& histogram = metricsService->createDoubleHistogram(
        MetricNames::kTest2, "description", MetricUnit::kSeconds, options);
    histogram.record(3.0);

    BSONObjBuilder builder;
    mongo::globalMetricTreeSet()[ClusterRole::None].appendTo(builder);
    const BSONObj obj = builder.obj();
    ASSERT_BSONOBJ_EQ(obj["metrics"]["ops"]["latencyHistogram"].Obj(),
                      BSON("average" << 3.0 << "count" << 1));
}

TEST_F(SerializeMetricsTreeTest, RoleShard) {
    CounterOptions options{.serverStatusOptions = ServerStatusOptions{
                               .dottedPath = "ingress.openConnections",
                               .role = ClusterRole::ShardServer,
                           }};
    auto& counter = metricsService->createInt64Counter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds, options);
    counter.add(11);

    BSONObjBuilder shardBuilder;
    mongo::globalMetricTreeSet()[ClusterRole::ShardServer].appendTo(shardBuilder);
    const BSONObj shardObj = shardBuilder.obj();
    ASSERT_EQ(shardObj["metrics"]["ingress"]["openConnections"].Long(), 11);

    BSONObjBuilder noneBuilder;
    mongo::globalMetricTreeSet()[ClusterRole::None].appendTo(noneBuilder);
    BSONObj noneObj = noneBuilder.obj();
    ASSERT_THAT(noneObj["metrics"],
                AnyOf(IsBSONElement(_, BSONType::eoo, _),
                      IsBSONElement(_,
                                    BSONType::object,
                                    Matcher<BSONObj>(Not(BSONObjElements(
                                        Contains(IsBSONElement("ingress", _, _))))))));

    BSONObjBuilder routerBuilder;
    mongo::globalMetricTreeSet()[ClusterRole::RouterServer].appendTo(routerBuilder);
    BSONObj routerObj = routerBuilder.obj();
    ASSERT_THAT(routerObj["metrics"],
                AnyOf(IsBSONElement(_, BSONType::eoo, _),
                      IsBSONElement(_,
                                    BSONType::object,
                                    Matcher<BSONObj>(Not(BSONObjElements(
                                        Contains(IsBSONElement("ingress", _, _))))))));
}

TEST_F(SerializeMetricsTreeTest, RoleRouter) {
    CounterOptions options{.serverStatusOptions = ServerStatusOptions{
                               .dottedPath = "ingress.openConnections",
                               .role = ClusterRole::RouterServer,
                           }};
    auto& counter = metricsService->createInt64Counter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds, options);
    counter.add(22);

    BSONObjBuilder routerBuilder;
    mongo::globalMetricTreeSet()[ClusterRole::RouterServer].appendTo(routerBuilder);
    const BSONObj routerObj = routerBuilder.obj();
    ASSERT_EQ(routerObj["metrics"]["ingress"]["openConnections"].Long(), 22);

    BSONObjBuilder noneBuilder;
    mongo::globalMetricTreeSet()[ClusterRole::None].appendTo(noneBuilder);
    BSONObj noneObj = noneBuilder.obj();
    ASSERT_THAT(noneObj["metrics"],
                AnyOf(IsBSONElement(_, BSONType::eoo, _),
                      IsBSONElement(_,
                                    BSONType::object,
                                    Matcher<BSONObj>(Not(BSONObjElements(
                                        Contains(IsBSONElement("ingress", _, _))))))));

    BSONObjBuilder shardBuilder;
    mongo::globalMetricTreeSet()[ClusterRole::ShardServer].appendTo(shardBuilder);
    BSONObj shardObj = shardBuilder.obj();
    ASSERT_THAT(shardObj["metrics"],
                AnyOf(IsBSONElement(_, BSONType::eoo, _),
                      IsBSONElement(_,
                                    BSONType::object,
                                    Matcher<BSONObj>(Not(BSONObjElements(
                                        Contains(IsBSONElement("ingress", _, _))))))));
}

TEST_F(SerializeMetricsTreeTest, RoleNone) {
    CounterOptions options{.serverStatusOptions = ServerStatusOptions{
                               .dottedPath = "ingress.openConnections",
                               .role = ClusterRole::None,
                           }};
    auto& counter = metricsService->createInt64Counter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds, options);
    counter.add(33);

    BSONObjBuilder noneBuilder;
    mongo::globalMetricTreeSet()[ClusterRole::None].appendTo(noneBuilder);
    ASSERT_EQ(noneBuilder.obj()["metrics"]["ingress"]["openConnections"].Long(), 33);

    BSONObjBuilder shardBuilder;
    mongo::globalMetricTreeSet()[ClusterRole::ShardServer].appendTo(shardBuilder);
    BSONObj shardObj = shardBuilder.obj();
    ASSERT_THAT(shardObj["metrics"],
                AnyOf(IsBSONElement(_, BSONType::eoo, _),
                      IsBSONElement(_,
                                    BSONType::object,
                                    Matcher<BSONObj>(Not(BSONObjElements(
                                        Contains(IsBSONElement("ingress", _, _))))))));

    BSONObjBuilder routerBuilder;
    mongo::globalMetricTreeSet()[ClusterRole::RouterServer].appendTo(routerBuilder);
    BSONObj routerObj = routerBuilder.obj();
    ASSERT_THAT(routerObj["metrics"],
                AnyOf(IsBSONElement(_, BSONType::eoo, _),
                      IsBSONElement(_,
                                    BSONType::object,
                                    Matcher<BSONObj>(Not(BSONObjElements(
                                        Contains(IsBSONElement("ingress", _, _))))))));
}

TEST_F(SerializeMetricsTreeTest, SamePathDifferentMetricNamesDifferentRoles) {
    const std::string dottedPath = "counter";

    CounterOptions shardOptions{.serverStatusOptions = ServerStatusOptions{
                                    .dottedPath = dottedPath, .role = ClusterRole::ShardServer}};
    auto& shardCounter = metricsService->createInt64Counter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds, shardOptions);
    shardCounter.add(7);

    CounterOptions routerOptions{.serverStatusOptions = ServerStatusOptions{
                                     .dottedPath = dottedPath, .role = ClusterRole::RouterServer}};
    auto& routerCounter = metricsService->createInt64Counter(
        MetricNames::kTest2, "description", MetricUnit::kSeconds, routerOptions);
    routerCounter.add(9);

    BSONObjBuilder shardBuilder;
    mongo::globalMetricTreeSet()[ClusterRole::ShardServer].appendTo(shardBuilder);
    ASSERT_EQ(shardBuilder.obj()["metrics"]["counter"].Long(), 7);

    BSONObjBuilder routerBuilder;
    mongo::globalMetricTreeSet()[ClusterRole::RouterServer].appendTo(routerBuilder);
    ASSERT_EQ(routerBuilder.obj()["metrics"]["counter"].Long(), 9);
}

TEST_F(SerializeMetricsTreeTest, SharedPrefixSiblingLeaves) {
    CounterOptions optionsA{.serverStatusOptions = ServerStatusOptions{
                                .dottedPath = "common.metricA",
                                .role = ClusterRole::None,
                            }};
    auto& counterA = metricsService->createInt64Counter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds, optionsA);
    counterA.add(10);

    CounterOptions optionsB{.serverStatusOptions = ServerStatusOptions{
                                .dottedPath = "common.metricB",
                                .role = ClusterRole::None,
                            }};
    auto& counterB = metricsService->createInt64Counter(
        MetricNames::kTest2, "description", MetricUnit::kSeconds, optionsB);
    counterB.add(20);

    BSONObjBuilder builder;
    mongo::globalMetricTreeSet()[ClusterRole::None].appendTo(builder);
    const BSONObj serverStatusObj = builder.obj();
    const BSONObj commonObj = serverStatusObj["metrics"]["common"].Obj();
    ASSERT_THAT(
        commonObj,
        BSONObjElements(UnorderedElementsAre(IsBSONElement("metricA", _, Matcher<long long>(10)),
                                             IsBSONElement("metricB", _, Matcher<long long>(20)))));
}

TEST_F(SerializeMetricsTreeTest, SharedPrefixShallowAndDeep) {
    CounterOptions shallowOptions{.serverStatusOptions = ServerStatusOptions{
                                      .dottedPath = "common.shallowMetric",
                                      .role = ClusterRole::None,
                                  }};
    auto& shallowCounter = metricsService->createInt64Counter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds, shallowOptions);
    shallowCounter.add(7);

    CounterOptions deepOptions{.serverStatusOptions = ServerStatusOptions{
                                   .dottedPath = "common.nested.deepMetric",
                                   .role = ClusterRole::None,
                               }};
    auto& deepCounter = metricsService->createInt64Counter(
        MetricNames::kTest2, "description", MetricUnit::kSeconds, deepOptions);
    deepCounter.add(8);

    BSONObjBuilder builder;
    mongo::globalMetricTreeSet()[ClusterRole::None].appendTo(builder);
    const BSONObj serverStatusObj = builder.obj();
    const BSONObj commonObj = serverStatusObj["metrics"]["common"].Obj();
    ASSERT_THAT(
        commonObj,
        BSONObjElements(UnorderedElementsAre(
            IsBSONElement("shallowMetric", _, Matcher<long long>(7)),
            IsBSONElement("nested", _, Matcher<BSONObj>(BSONObjEQ(BSON("deepMetric" << 8)))))));
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
using ClearForTestsTest = MetricsServiceTest;

TEST_F(ClearForTestsTest, RemovesFromBothMetricsServiceAndServerStatusTree) {
    CounterOptions options{.serverStatusOptions = ServerStatusOptions{
                               .dottedPath = "ingress.openConnections",
                               .role = ClusterRole::None,
                           }};
    auto& counter = metricsService->createInt64Counter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds, options);
    counter.add(11);

    metricsService->clearForTests();

    OtelMetricsCapturer metricsCapturer(*metricsService);
    if (metricsCapturer.canReadMetrics()) {
        ASSERT_THROWS_CODE(metricsCapturer.readInt64Counter(MetricNames::kTest1),
                           DBException,
                           ErrorCodes::KeyNotFound);
    }
    {
        BSONObjBuilder builder;
        mongo::globalMetricTreeSet()[ClusterRole::None].appendTo(builder);
        ASSERT_THAT(builder.obj()["metrics"],
                    AnyOf(IsBSONElement(_, BSONType::eoo, _),
                          IsBSONElement(_,
                                        BSONType::object,
                                        Matcher<BSONObj>(Not(BSONObjElements(
                                            Contains(IsBSONElement("ingress", _, _))))))));
    }
}

TEST_F(ClearForTestsTest, RemovesFromAllServerStatusTrees) {
    struct RoleAndMetricName {
        ClusterRole role;
        const MetricName& name;
    };
    for (auto [role, name] : {RoleAndMetricName{ClusterRole::None, MetricNames::kTest1},
                              RoleAndMetricName{ClusterRole::ShardServer, MetricNames::kTest2},
                              RoleAndMetricName{ClusterRole::RouterServer, MetricNames::kTest3}}) {
        CounterOptions options{.serverStatusOptions = ServerStatusOptions{
                                   .dottedPath = "ingress.openConnections",
                                   .role = role,
                               }};
        auto& counter = metricsService->createInt64Counter(
            name, "description", MetricUnit::kOperations, options);
        counter.add(11);

        BSONObjBuilder builder;
        mongo::globalMetricTreeSet()[ClusterRole(role)].appendTo(builder);
        ASSERT_EQ(builder.obj()["metrics"]["ingress"]["openConnections"].Long(), 11);
    }

    metricsService->clearForTests();

    for (auto role : {ClusterRole::None, ClusterRole::ShardServer, ClusterRole::RouterServer}) {
        BSONObjBuilder builder;
        mongo::globalMetricTreeSet()[ClusterRole(role)].appendTo(builder);
        ASSERT_THAT(builder.obj()["metrics"],
                    AnyOf(IsBSONElement(_, BSONType::eoo, _),
                          IsBSONElement(_,
                                        BSONType::object,
                                        Matcher<BSONObj>(Not(BSONObjElements(
                                            Contains(IsBSONElement("ingress", _, _))))))));
    }
}

TEST_F(ClearForTestsTest, ClearsObservableCallbacks) {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    if (!metricsCapturer.canReadMetrics()) {
        return;
    }
    auto& counter = metricsService->createInt64Counter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);
    counter.add(11);
    ASSERT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 11);

    metricsService->clearForTests();

    // Re-register the same metric name. If the old observable callback was not cleared, triggering
    // an export would invoke it with a dangling pointer (crash), or report the stale value 11.
    metricsService->createInt64Counter(MetricNames::kTest1, "description", MetricUnit::kSeconds);
    ASSERT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 0);
}

TEST_F(ClearForTestsTest, AllowsReregistrationWithDifferentOptions) {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    metricsService->createInt64Counter(MetricNames::kTest1, "description", MetricUnit::kSeconds);
    metricsService->clearForTests();
    auto& counter =
        metricsService->createInt64Counter(MetricNames::kTest1, "description", MetricUnit::kBytes);
    counter.add(5);
    if (metricsCapturer.canReadMetrics()) {
        EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kTest1), 5);
    }
}

TEST_F(ClearForTestsTest, AllowsReregistrationWithDifferentType) {
    OtelMetricsCapturer metricsCapturer(*metricsService);
    metricsService->createInt64Counter(MetricNames::kTest1, "description", MetricUnit::kSeconds);
    metricsService->clearForTests();
    auto& counter = metricsService->createDoubleCounter(
        MetricNames::kTest1, "description", MetricUnit::kSeconds);
    counter.add(5.0);
    if (metricsCapturer.canReadMetrics()) {
        EXPECT_DOUBLE_EQ(metricsCapturer.readDoubleCounter(MetricNames::kTest1), 5.0);
    }
}

}  // namespace
}  // namespace mongo::otel::metrics
