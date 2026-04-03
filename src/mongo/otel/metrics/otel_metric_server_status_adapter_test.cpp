/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/otel/metrics/otel_metric_server_status_adapter.h"

#include "mongo/base/error_codes.h"
#include "mongo/config.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/otel/metrics/metrics_updown_counter.h"
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
        boost::none);
#else
    return std::make_unique<HistogramImpl<T>>();
#endif  // MONGO_CONFIG_OTEL
}

using ScalarNumericTypes = testing::Types<int64_t, double>;

template <typename T>
class OtelAdapterCounterScalarTest : public testing::Test {};

TYPED_TEST_SUITE(OtelAdapterCounterScalarTest, ScalarNumericTypes);

TYPED_TEST(OtelAdapterCounterScalarTest, AppendTo) {
    CounterImpl<TypeParam> counter;
    if constexpr (std::is_same_v<TypeParam, int64_t>) {
        counter.add(42);
    } else {
        counter.add(1.25);
    }
    OtelMetricServerStatusAdapter adapter(&counter);

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
    GaugeImpl<TypeParam> gauge;
    if constexpr (std::is_same_v<TypeParam, int64_t>) {
        gauge.set(-3);
    } else {
        gauge.set(-1.5);
    }
    OtelMetricServerStatusAdapter adapter(&gauge);

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
    UpDownCounterImpl<TypeParam> upDown;
    if constexpr (std::is_same_v<TypeParam, int64_t>) {
        upDown.add(5);
        upDown.add(-2);
    } else {
        upDown.add(2.5);
        upDown.add(-0.5);
    }
    OtelMetricServerStatusAdapter adapter(&upDown);

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
    CounterImpl<int64_t> counter;
    counter.add(1);
    OtelMetricServerStatusAdapter adapter(&counter);
    adapter.setEnabledPredicate([] { return false; });

    BSONObjBuilder bob;
    adapter.appendTo(bob, "openConnections");
    ASSERT_BSONOBJ_EQ(bob.obj(), BSONObj());
}

TEST(OtelMetricServerStatusAdapter, AppendsToNonEmptyBuilder) {
    CounterImpl<int64_t> counter;
    counter.add(99);
    OtelMetricServerStatusAdapter adapter(&counter);

    BSONObjBuilder bob;
    bob.append("openConnections", 1);
    adapter.appendTo(bob, "closedConnections");
    ASSERT_BSONOBJ_EQ(bob.obj(), BSON("openConnections" << 1 << "closedConnections" << 99));
}

TEST(OtelMetricServerStatusAdapter, MetricTreeAddAndAppendTo) {
    CounterImpl<int64_t> counter;
    counter.add(7);
    auto adapter = std::make_unique<OtelMetricServerStatusAdapter>(&counter);

    MetricTree tree;
    tree.add("ingress.openConnections", std::move(adapter));

    BSONObjBuilder section;
    tree.appendTo(section);
    ASSERT_BSONOBJ_EQ(section.obj(),
                      BSON("metrics" << BSON("ingress" << BSON("openConnections" << 7))));
}

}  // namespace

}  // namespace mongo::otel::metrics
