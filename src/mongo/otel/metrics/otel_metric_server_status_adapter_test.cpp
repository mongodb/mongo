// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/otel_metric_server_status_adapter.h"

#include "mongo/base/error_codes.h"
#include "mongo/config.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/otel/metrics/metrics_scalar_metric.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <type_traits>

#include <boost/optional.hpp>

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/provider.h>
#endif  // MONGO_CONFIG_OTEL

namespace mongo::otel::metrics {
namespace {

/*
 * Metric that does not use the provided key field when serializing to BSON.
 */
class MisnamedSerializeMetric final : public Metric {
public:
    BSONObj serializeToBson(const std::string& key) const override {
        return BSON("wrongField" << 1);
    }
#ifdef MONGO_CONFIG_OTEL
    void reset(opentelemetry::metrics::Meter* meter) override {
        invariant(!meter);
    }
#endif  // MONGO_CONFIG_OTEL
};

template <typename T>
std::unique_ptr<HistogramImpl<T>> createHistogramForTest() {
#ifdef MONGO_CONFIG_OTEL
    return std::make_unique<HistogramImpl<T>>(
        *opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("test_meter"),
        "name",
        "description",
        "unit",
        HistogramSerializationFormat::kAverage);
#else
    return std::make_unique<HistogramImpl<T>>(HistogramSerializationFormat::kAverage);
#endif  // MONGO_CONFIG_OTEL
}

using ScalarNumericTypes = testing::Types<int64_t, double>;

template <typename T>
class OtelAdapterCounterScalarTest : public testing::Test {};

TYPED_TEST_SUITE(OtelAdapterCounterScalarTest, ScalarNumericTypes);

TYPED_TEST(OtelAdapterCounterScalarTest, AppendTo) {
    ScalarMetricImpl<TypeParam> impl;
    Counter<TypeParam>& counter = impl;
    if constexpr (std::is_same_v<TypeParam, int64_t>) {
        counter.add(42);
    } else {
        counter.add(1.25);
    }
    OtelMetricServerStatusAdapter adapter(&impl);

    BSONObjBuilder bob;
    adapter.appendTo(bob, "openConnections");
    if constexpr (std::is_same_v<TypeParam, int64_t>) {
        ASSERT_BSONOBJ_EQ(bob.obj(), BSON("openConnections" << 42));
    } else {
        ASSERT_BSONOBJ_EQ(bob.obj(), BSON("openConnections" << 1.25));
    }
}

template <typename T>
class OtelAdapterGaugeScalarTest : public testing::Test {};

TYPED_TEST_SUITE(OtelAdapterGaugeScalarTest, ScalarNumericTypes);

TYPED_TEST(OtelAdapterGaugeScalarTest, AppendTo) {
    ScalarMetricImpl<TypeParam> impl;
    Gauge<TypeParam>& gauge = impl;
    if constexpr (std::is_same_v<TypeParam, int64_t>) {
        gauge.set(-3);
    } else {
        gauge.set(-1.5);
    }
    OtelMetricServerStatusAdapter adapter(&impl);

    BSONObjBuilder bob;
    adapter.appendTo(bob, "openConnections");
    if constexpr (std::is_same_v<TypeParam, int64_t>) {
        ASSERT_BSONOBJ_EQ(bob.obj(), BSON("openConnections" << -3));
    } else {
        ASSERT_BSONOBJ_EQ(bob.obj(), BSON("openConnections" << -1.5));
    }
}

template <typename T>
class OtelAdapterUpDownCounterScalarTest : public testing::Test {};

TYPED_TEST_SUITE(OtelAdapterUpDownCounterScalarTest, ScalarNumericTypes);

TYPED_TEST(OtelAdapterUpDownCounterScalarTest, AppendTo) {
    ScalarMetricImpl<TypeParam> impl;
    UpDownCounter<TypeParam>& upDown = impl;
    if constexpr (std::is_same_v<TypeParam, int64_t>) {
        upDown.add(5);
        upDown.add(-2);
    } else {
        upDown.add(2.5);
        upDown.add(-0.5);
    }
    OtelMetricServerStatusAdapter adapter(&impl);

    BSONObjBuilder bob;
    adapter.appendTo(bob, "openConnections");
    if constexpr (std::is_same_v<TypeParam, int64_t>) {
        ASSERT_BSONOBJ_EQ(bob.obj(), BSON("openConnections" << 3));
    } else {
        ASSERT_BSONOBJ_EQ(bob.obj(), BSON("openConnections" << 2.0));
    }
}

using HistogramValueTypes = testing::Types<int64_t, double>;

template <typename T>
class OtelAdapterHistogramTest : public testing::Test {};

TYPED_TEST_SUITE(OtelAdapterHistogramTest, HistogramValueTypes);

TYPED_TEST(OtelAdapterHistogramTest, AppendTo) {
    auto histogram = createHistogramForTest<TypeParam>();
    if constexpr (std::is_same_v<TypeParam, double>) {
        histogram->record(3.0);
        histogram->record(5.0);
    } else {
        histogram->record(7);
    }
    OtelMetricServerStatusAdapter adapter(histogram.get());

    const std::string key = "openConnections";
    BSONObjBuilder bob;
    adapter.appendTo(bob, key);
    ASSERT_BSONOBJ_EQ(bob.obj(), histogram->serializeToBson(key));
}

TEST(OtelMetricServerStatusAdapter, ThrowsWhenAppendToOmitsLeafName) {
    MisnamedSerializeMetric metric;
    OtelMetricServerStatusAdapter adapter(&metric);
    BSONObjBuilder bob;
    ASSERT_THROWS_CODE(
        adapter.appendTo(bob, "openConnections"), DBException, ErrorCodes::KeyNotFound);
}

TEST(OtelMetricServerStatusAdapter, RespectsEnabledPredicate) {
    ScalarMetricImpl<int64_t> impl;
    Counter<int64_t>& counter = impl;
    counter.add(1);
    OtelMetricServerStatusAdapter adapter(&impl);
    adapter.setEnabledPredicate([] { return false; });

    BSONObjBuilder bob;
    adapter.appendTo(bob, "openConnections");
    ASSERT_BSONOBJ_EQ(bob.obj(), BSONObj());
}

TEST(OtelMetricServerStatusAdapter, AppendsToNonEmptyBuilder) {
    ScalarMetricImpl<int64_t> impl;
    Counter<int64_t>& counter = impl;
    counter.add(99);
    OtelMetricServerStatusAdapter adapter(&impl);

    BSONObjBuilder bob;
    bob.append("openConnections", 1);
    adapter.appendTo(bob, "closedConnections");
    ASSERT_BSONOBJ_EQ(bob.obj(), BSON("openConnections" << 1 << "closedConnections" << 99));
}

TEST(OtelMetricServerStatusAdapter, MetricTreeAddAndAppendTo) {
    ScalarMetricImpl<int64_t> impl;
    Counter<int64_t>& counter = impl;
    counter.add(7);
    auto adapter = std::make_unique<OtelMetricServerStatusAdapter>(&impl);

    MetricTree tree;
    tree.add("ingress.openConnections", std::move(adapter));

    BSONObjBuilder section;
    tree.appendTo(section);
    ASSERT_BSONOBJ_EQ(section.obj(),
                      BSON("metrics" << BSON("ingress" << BSON("openConnections" << 7))));
}

}  // namespace
}  // namespace mongo::otel::metrics
