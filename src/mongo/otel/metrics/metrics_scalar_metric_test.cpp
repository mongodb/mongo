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

#include "mongo/otel/metrics/metrics_scalar_metric.h"

#include "mongo/config.h"
#include "mongo/otel/metrics/metrics_attributes_test_utils.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace mongo::otel::metrics {
namespace {
using testing::_;
using testing::DoubleEq;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::UnorderedElementsAre;

///////////////////////////////////////////////////////////////////////////////
// Counter tests
///////////////////////////////////////////////////////////////////////////////

template <typename T>
class ScalarMetricImplTest : public testing::Test {};

using ScalarMetricTypes = testing::Types<int64_t, double>;
TYPED_TEST_SUITE(ScalarMetricImplTest, ScalarMetricTypes);

TYPED_TEST(ScalarMetricImplTest, Adds) {
    ScalarMetricImpl<TypeParam> impl;
    Counter<TypeParam>& counter = impl;
    // Initial value is zero for a metric with no attributes.
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    counter.add(1);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 1)));
    counter.add(10);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 11)));
}

TYPED_TEST(ScalarMetricImplTest, AddsZero) {
    ScalarMetricImpl<TypeParam> impl;
    Counter<TypeParam>& counter = impl;
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    counter.add(0);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    counter.add(10);
    counter.add(0);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 10)));
}

TYPED_TEST(ScalarMetricImplTest, ExceptionOnNegativeAdd) {
    ScalarMetricImpl<TypeParam> impl;
    Counter<TypeParam>& counter = impl;
    counter.add(1);
    ASSERT_THROWS_CODE(counter.add(-1), DBException, ErrorCodes::BadValue);
}


TYPED_TEST(ScalarMetricImplTest, Serialization) {
    ScalarMetricImpl<TypeParam> impl;
    Counter<TypeParam>& counter = impl;
    const std::string key = "a";
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 0));
    counter.add(0);
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 0));
    counter.add(10);
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 10));
}

TYPED_TEST(ScalarMetricImplTest, SerializationWithAttributesAggregates) {
    ScalarMetricImpl<TypeParam, int32_t> impl({.name = "is_cool", .values = {1, 2, 3}});
    Counter<TypeParam, int32_t>& counter = impl;
    const std::string key = "a";
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 0));
    counter.add(0, {1});
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 0));
    counter.add(1, {2});
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 1));
    counter.add(10, {3});
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 11));
}

TYPED_TEST(ScalarMetricImplTest, ValuesSkipsZeroForAttributedCounter) {
    ScalarMetricImpl<TypeParam, bool> impl({.name = "is_cool", .values = {true, false}});
    Counter<TypeParam, bool>& counter = impl;
    EXPECT_THAT(impl.values(), IsEmpty());
    counter.add(0, {true});
    EXPECT_THAT(impl.values(), IsEmpty());
}

TYPED_TEST(ScalarMetricImplTest, ValuesNoAttributes) {
    ScalarMetricImpl<TypeParam> impl;
    Counter<TypeParam>& counter = impl;
    counter.add(5);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 5)));
}

TYPED_TEST(ScalarMetricImplTest, ThrowsIfAttributeNamesDuplicated) {
    ASSERT_THROWS_CODE((ScalarMetricImpl<TypeParam, bool, bool, bool, bool>(
                           {.name = "is_cool1", .values = {true}},
                           {.name = "is_cool2", .values = {true}},
                           {.name = "is_cool3", .values = {true}},
                           {.name = "is_cool2", .values = {true}})),
                       DBException,
                       ErrorCodes::BadValue);
}

TYPED_TEST(ScalarMetricImplTest, ThrowsOnInvalidAttributes) {
    ASSERT_THROWS_CODE(
        (ScalarMetricImpl<TypeParam, bool>({.name = "is_cool", .values = {true, true}})),
        DBException,
        ErrorCodes::BadValue);
}

TYPED_TEST(ScalarMetricImplTest, AddWithSingleAttribute) {
    ScalarMetricImpl<TypeParam, bool> impl({.name = "is_cool", .values = {true, false}});
    Counter<TypeParam, bool>& counter = impl;

    counter.add(5, {true});
    counter.add(3, {false});
    counter.add(2, {true});

    EXPECT_THAT(impl.values(),
                UnorderedElementsAre(
                    IsAttributesAndValue(
                        ElementsAre(AttributeNameAndValue{.name = "is_cool", .value = true}), 7),
                    IsAttributesAndValue(
                        ElementsAre(AttributeNameAndValue{.name = "is_cool", .value = false}), 3)));
}

TYPED_TEST(ScalarMetricImplTest, AddWithMultipleAttributes) {
    ScalarMetricImpl<TypeParam, bool, int64_t> impl({.name = "is_cool", .values = {true, false}},
                                                    {.name = "size", .values = {1, 2}});
    Counter<TypeParam, bool, int64_t>& counter = impl;

    counter.add(5, {true, 1});
    counter.add(3, {false, 2});

    EXPECT_THAT(
        impl.values(),
        UnorderedElementsAre(
            IsAttributesAndValue(
                UnorderedElementsAre(AttributeNameAndValue{.name = "is_cool", .value = true},
                                     AttributeNameAndValue{.name = "size", .value = int64_t{1}}),
                5),
            IsAttributesAndValue(
                UnorderedElementsAre(AttributeNameAndValue{.name = "is_cool", .value = false},
                                     AttributeNameAndValue{.name = "size", .value = int64_t{2}}),
                3)));
}

TYPED_TEST(ScalarMetricImplTest, ExceptionOnUndeclaredAttributes) {
    ScalarMetricImpl<TypeParam, int64_t> impl({.name = "size", .values = {1, 2}});
    Counter<TypeParam, int64_t>& counter = impl;

    ASSERT_THROWS_CODE(counter.add(1, {3}), DBException, ErrorCodes::BadValue);
}

TYPED_TEST(ScalarMetricImplTest, ValuesSkipsZeroAttributes) {
    ScalarMetricImpl<TypeParam, bool> impl({.name = "is_cool", .values = {true, false}});
    Counter<TypeParam, bool>& counter = impl;

    counter.add(5, {true});

    EXPECT_THAT(impl.values(),
                ElementsAre(IsAttributesAndValue(
                    ElementsAre(AttributeNameAndValue{.name = "is_cool", .value = true}), 5)));
}

TYPED_TEST(ScalarMetricImplTest, StringDataAttributeValueIsCopied) {
    auto sourceValues = std::make_unique<std::vector<std::string>>(
        std::initializer_list<std::string>{"foo", "bar"});
    ScalarMetricImpl<TypeParam, StringData> impl(
        {.name = "temperature", .values = {(*sourceValues)[0], (*sourceValues)[1]}});
    Counter<TypeParam, StringData>& counter = impl;
    sourceValues = nullptr;

    counter.add(5, {"foo"_sd});
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(_, 5)));
}

TYPED_TEST(ScalarMetricImplTest, SpanAttributeValueIsCopied) {
    auto sourceIntData = std::make_unique<std::vector<std::vector<int32_t>>>(
        std::vector<std::vector<int32_t>>{{1, 2}, {3, 4}});
    auto string1 = std::make_unique<std::string>("a");
    auto string2 = std::make_unique<std::string>("b");
    auto sourceStringData = std::make_unique<std::vector<std::vector<StringData>>>(
        std::vector<std::vector<StringData>>{{*string1}, {*string1, *string2}});
    ScalarMetricImpl<TypeParam, std::span<int32_t>, std::span<StringData>> impl(
        {.name = "intData",
         .values = {std::span<int32_t>((*sourceIntData)[0]),
                    std::span<int32_t>((*sourceIntData)[1])}},
        {.name = "stringData",
         .values = {std::span<StringData>((*sourceStringData)[0]),
                    std::span<StringData>((*sourceStringData)[1])}});
    Counter<TypeParam, std::span<int32_t>, std::span<StringData>>& counter = impl;
    sourceIntData = nullptr;
    string1 = nullptr;
    string2 = nullptr;
    sourceStringData = nullptr;

    std::vector<int32_t> intInput{1, 2};
    std::vector<StringData> stringInput{"a"_sd, "b"_sd};
    counter.add(5, {std::span<int32_t>(intInput), std::span<StringData>(stringInput)});
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(_, 5)));
}

TEST(DoubleScalarMetricImplTest, AddsFractionalValues) {
    ScalarMetricImpl<double, bool> impl({.name = "is_cool", .values = {true, false}});
    Counter<double, bool>& counter = impl;

    counter.add(1.1, {true});
    counter.add(10.5, {false});
    counter.add(2.2, {true});

    EXPECT_THAT(impl.values(),
                UnorderedElementsAre(IsAttributesAndValue(ElementsAre(AttributeNameAndValue{
                                                              .name = "is_cool", .value = true}),
                                                          DoubleEq(3.3)),
                                     IsAttributesAndValue(ElementsAre(AttributeNameAndValue{
                                                              .name = "is_cool", .value = false}),
                                                          DoubleEq(10.5))));
}

///////////////////////////////////////////////////////////////////////////////
// Gauge tests
///////////////////////////////////////////////////////////////////////////////

template <typename T>
class GaugeTest : public testing::Test {};

using GaugeTypes = testing::Types<int64_t, double>;
TYPED_TEST_SUITE(GaugeTest, GaugeTypes);

TYPED_TEST(GaugeTest, Sets) {
    ScalarMetricImpl<TypeParam> impl;
    Gauge<TypeParam>& gauge = impl;
    // Initial value is zero for a metric with no attributes.
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    gauge.set(1);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 1)));
    // set() replaces the previous value rather than accumulating.
    gauge.set(10);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 10)));
    gauge.set(-1);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), -1)));
}

TYPED_TEST(GaugeTest, Serialization) {
    ScalarMetricImpl<TypeParam> impl;
    Gauge<TypeParam>& gauge = impl;
    const std::string key = "a";
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 0));
    gauge.set(10);
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 10));
    gauge.set(-3);
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << -3));
}

TYPED_TEST(GaugeTest, SetWithSingleAttribute) {
    ScalarMetricImpl<TypeParam, bool> impl({.name = "is_primary", .values = {true, false}});
    Gauge<TypeParam, bool>& gauge = impl;

    gauge.set(42, {true});
    gauge.set(7, {false});
    gauge.set(99, {true});

    EXPECT_THAT(
        impl.values(),
        UnorderedElementsAre(
            IsAttributesAndValue(
                ElementsAre(AttributeNameAndValue{.name = "is_primary", .value = true}), 99),
            IsAttributesAndValue(
                ElementsAre(AttributeNameAndValue{.name = "is_primary", .value = false}), 7)));
}

TYPED_TEST(GaugeTest, ValuesSkipsZeroForAttributedGauge) {
    ScalarMetricImpl<TypeParam, bool> impl({.name = "is_primary", .values = {true, false}});
    EXPECT_THAT(impl.values(), IsEmpty());
}

TYPED_TEST(GaugeTest, ExceptionOnUndeclaredAttributes) {
    ScalarMetricImpl<TypeParam, int64_t> impl({.name = "size", .values = {1, 2}});
    Gauge<TypeParam, int64_t>& gauge = impl;

    ASSERT_THROWS_CODE(gauge.set(1, {3}), DBException, ErrorCodes::BadValue);
}

TEST(DoubleGaugeTest, SetsFractionalValues) {
    ScalarMetricImpl<double> impl;
    Gauge<double>& gauge = impl;
    gauge.set(1.1);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(1.1))));
    gauge.set(-3.14);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(-3.14))));
}

///////////////////////////////////////////////////////////////////////////////
// GaugeImpl tests
///////////////////////////////////////////////////////////////////////////////

template <typename T>
class GaugeImplTest : public testing::Test {};

TYPED_TEST_SUITE(GaugeImplTest, GaugeTypes);

TYPED_TEST(GaugeImplTest, SetIfLess) {
    GaugeImpl<TypeParam> gauge{std::numeric_limits<TypeParam>::max()};
    gauge.setIfLess(10);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 10)));
    gauge.setIfLess(5);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 5)));
    gauge.setIfLess(10);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 5)));
    gauge.setIfLess(5);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 5)));
}

TYPED_TEST(GaugeImplTest, SetIfGreater) {
    GaugeImpl<TypeParam> gauge{std::numeric_limits<TypeParam>::lowest()};
    gauge.setIfGreater(5);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 5)));
    gauge.setIfGreater(10);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 10)));
    gauge.setIfGreater(5);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 10)));
    gauge.setIfGreater(10);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 10)));
}

#ifdef MONGO_CONFIG_OTEL
TYPED_TEST(GaugeImplTest, ResetRestoresInitialValue) {
    GaugeImpl<TypeParam> minGauge{std::numeric_limits<TypeParam>::max()};
    minGauge.setIfLess(5);
    minGauge.reset(nullptr);
    EXPECT_THAT(
        minGauge.values(),
        ElementsAre(IsAttributesAndValue(IsEmpty(), std::numeric_limits<TypeParam>::max())));

    GaugeImpl<TypeParam> maxGauge{std::numeric_limits<TypeParam>::lowest()};
    maxGauge.setIfGreater(5);
    maxGauge.reset(nullptr);
    EXPECT_THAT(
        maxGauge.values(),
        ElementsAre(IsAttributesAndValue(IsEmpty(), std::numeric_limits<TypeParam>::lowest())));
}
#endif  // MONGO_CONFIG_OTEL


///////////////////////////////////////////////////////////////////////////////
// MinGauge tests
///////////////////////////////////////////////////////////////////////////////

template <typename T>
class MinGaugeTest : public testing::Test {};

TYPED_TEST_SUITE(MinGaugeTest, GaugeTypes);

TYPED_TEST(MinGaugeTest, InitialValueIsSentinel) {
    GaugeImpl<TypeParam> impl{std::numeric_limits<TypeParam>::max()};
    MinGauge<TypeParam>& gauge = impl;
    EXPECT_THAT(
        gauge.values(),
        ElementsAre(IsAttributesAndValue(IsEmpty(), std::numeric_limits<TypeParam>::max())));
}

TYPED_TEST(MinGaugeTest, SetIfLessThroughInterface) {
    GaugeImpl<TypeParam> impl{std::numeric_limits<TypeParam>::max()};
    MinGauge<TypeParam>& gauge = impl;
    gauge.setIfLess(10);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 10)));
    gauge.setIfLess(5);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 5)));
    gauge.setIfLess(20);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 5)));
}

TYPED_TEST(MinGaugeTest, SetOverwritesUnconditionally) {
    GaugeImpl<TypeParam> impl{std::numeric_limits<TypeParam>::max()};
    MinGauge<TypeParam>& gauge = impl;
    gauge.setIfLess(5);
    gauge.set(100);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 100)));
}

///////////////////////////////////////////////////////////////////////////////
// MaxGauge tests
///////////////////////////////////////////////////////////////////////////////

template <typename T>
class MaxGaugeTest : public testing::Test {};

TYPED_TEST_SUITE(MaxGaugeTest, GaugeTypes);

TYPED_TEST(MaxGaugeTest, InitialValueIsSentinel) {
    GaugeImpl<TypeParam> impl{std::numeric_limits<TypeParam>::lowest()};
    MaxGauge<TypeParam>& gauge = impl;
    EXPECT_THAT(
        gauge.values(),
        ElementsAre(IsAttributesAndValue(IsEmpty(), std::numeric_limits<TypeParam>::lowest())));
}

TYPED_TEST(MaxGaugeTest, SetIfGreaterThroughInterface) {
    GaugeImpl<TypeParam> impl{std::numeric_limits<TypeParam>::lowest()};
    MaxGauge<TypeParam>& gauge = impl;
    gauge.setIfGreater(5);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 5)));
    gauge.setIfGreater(10);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 10)));
    gauge.setIfGreater(3);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 10)));
}

TYPED_TEST(MaxGaugeTest, SetOverwritesUnconditionally) {
    GaugeImpl<TypeParam> impl{std::numeric_limits<TypeParam>::lowest()};
    MaxGauge<TypeParam>& gauge = impl;
    gauge.setIfGreater(100);
    gauge.set(1);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 1)));
}

///////////////////////////////////////////////////////////////////////////////
// UpDownCounter tests
///////////////////////////////////////////////////////////////////////////////

template <typename T>
class UpDownCounterTest : public testing::Test {};

using UpDownCounterTypes = testing::Types<int64_t, double>;
TYPED_TEST_SUITE(UpDownCounterTest, UpDownCounterTypes);

TYPED_TEST(UpDownCounterTest, Adds) {
    ScalarMetricImpl<TypeParam> impl;
    UpDownCounter<TypeParam>& counter = impl;
    // Initial value is zero for a metric with no attributes.
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    counter.add(1);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 1)));
    counter.add(10);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 11)));
}

TYPED_TEST(UpDownCounterTest, AddsZero) {
    ScalarMetricImpl<TypeParam> impl;
    UpDownCounter<TypeParam>& counter = impl;
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    counter.add(0);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    counter.add(10);
    counter.add(0);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 10)));
}

TYPED_TEST(UpDownCounterTest, Subtracts) {
    ScalarMetricImpl<TypeParam> impl;
    UpDownCounter<TypeParam>& counter = impl;
    counter.add(5);
    counter.add(-2);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 3)));
    counter.add(-3);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
}


TYPED_TEST(UpDownCounterTest, Serialization) {
    ScalarMetricImpl<TypeParam> impl;
    UpDownCounter<TypeParam>& counter = impl;
    const std::string key = "a";
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 0));
    counter.add(0);
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 0));
    counter.add(10);
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 10));
    counter.add(-4);
    ASSERT_BSONOBJ_EQ(impl.serializeToBson(key), BSON(key << 6));
}

TYPED_TEST(UpDownCounterTest, AddWithSingleAttribute) {
    ScalarMetricImpl<TypeParam, bool> impl({.name = "is_primary", .values = {true, false}});
    UpDownCounter<TypeParam, bool>& counter = impl;

    counter.add(5, {true});
    counter.add(-2, {true});
    counter.add(3, {false});

    EXPECT_THAT(
        impl.values(),
        UnorderedElementsAre(
            IsAttributesAndValue(
                ElementsAre(AttributeNameAndValue{.name = "is_primary", .value = true}), 3),
            IsAttributesAndValue(
                ElementsAre(AttributeNameAndValue{.name = "is_primary", .value = false}), 3)));
}

TYPED_TEST(UpDownCounterTest, ValuesSkipsZeroForAttributedCounter) {
    ScalarMetricImpl<TypeParam, bool> impl({.name = "is_primary", .values = {true, false}});
    EXPECT_THAT(impl.values(), IsEmpty());
}

TYPED_TEST(UpDownCounterTest, ExceptionOnUndeclaredAttributes) {
    ScalarMetricImpl<TypeParam, int64_t> impl({.name = "size", .values = {1, 2}});
    UpDownCounter<TypeParam, int64_t>& counter = impl;

    ASSERT_THROWS_CODE(counter.add(1, {3}), DBException, ErrorCodes::BadValue);
}

TEST(DoubleUpDownCounterTest, AddsFractionalValues) {
    ScalarMetricImpl<double> impl;
    UpDownCounter<double>& counter = impl;
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(0.0))));
    counter.add(1.1);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(1.1))));
    counter.add(10.5);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(11.6))));
    counter.add(-0.6);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(11.0))));
}

///////////////////////////////////////////////////////////////////////////////
// ReportingPolicy tests
///////////////////////////////////////////////////////////////////////////////

template <typename T>
class ScalarMetricImplReportingPolicyGlobal : public testing::Test {};

template <typename T>
class ScalarMetricImplReportingPolicyPerCombination : public testing::Test {};

TYPED_TEST_SUITE(ScalarMetricImplReportingPolicyGlobal, ScalarMetricTypes);
TYPED_TEST_SUITE(ScalarMetricImplReportingPolicyPerCombination, ScalarMetricTypes);

TYPED_TEST(ScalarMetricImplReportingPolicyGlobal, KUnconditionallyIncludesAllCombinations) {
    ScalarMetricImpl<TypeParam, bool, int64_t> impl(ReportingPolicy::kUnconditionally,
                                                    {.name = "is_cool", .values = {true, false}},
                                                    {.name = "size", .values = {1, 2}});
    Counter<TypeParam, bool, int64_t>& counter = impl;
    counter.add(5, {true, 1});
    EXPECT_THAT(
        impl.values(),
        UnorderedElementsAre(
            IsAttributesAndValue(
                UnorderedElementsAre(AttributeNameAndValue{.name = "is_cool", .value = true},
                                     AttributeNameAndValue{.name = "size", .value = int64_t{1}}),
                5),
            IsAttributesAndValue(
                UnorderedElementsAre(AttributeNameAndValue{.name = "is_cool", .value = true},
                                     AttributeNameAndValue{.name = "size", .value = int64_t{2}}),
                0),
            IsAttributesAndValue(
                UnorderedElementsAre(AttributeNameAndValue{.name = "is_cool", .value = false},
                                     AttributeNameAndValue{.name = "size", .value = int64_t{1}}),
                0),
            IsAttributesAndValue(
                UnorderedElementsAre(AttributeNameAndValue{.name = "is_cool", .value = false},
                                     AttributeNameAndValue{.name = "size", .value = int64_t{2}}),
                0)));
}

#ifdef MONGO_CONFIG_OTEL
TYPED_TEST(ScalarMetricImplReportingPolicyGlobal,
           KIfCurrentlyNonZeroExcludesCombinationsAfterReset) {
    ScalarMetricImpl<TypeParam, bool> impl(ReportingPolicy::kIfCurrentlyNonZero,
                                           {.name = "is_cool", .values = {true, false}});
    Counter<TypeParam, bool>& counter = impl;
    counter.add(5, {true});
    impl.reset(nullptr);
    EXPECT_THAT(impl.values(), IsEmpty());
}
#endif  // MONGO_CONFIG_OTEL

TYPED_TEST(ScalarMetricImplReportingPolicyGlobal, KIfCurrentlyNonZeroExcludesZeroNoAttributeImpl) {
    ScalarMetricImpl<TypeParam> impl(ReportingPolicy::kIfCurrentlyNonZero);
    Counter<TypeParam>& counter = impl;
    EXPECT_THAT(impl.values(), IsEmpty());
    counter.add(5);
    EXPECT_THAT(impl.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 5)));
}

#ifdef MONGO_CONFIG_OTEL
TYPED_TEST(ScalarMetricImplReportingPolicyGlobal,
           KIfCurrentlyNonZeroExcludesZeroAfterResetNoAttributeImpl) {
    ScalarMetricImpl<TypeParam> impl(ReportingPolicy::kIfCurrentlyNonZero);
    Counter<TypeParam>& counter = impl;
    counter.add(5);
    impl.reset(nullptr);
    EXPECT_THAT(impl.values(), IsEmpty());
}
#endif  // MONGO_CONFIG_OTEL

// TODO SERVER-124075: Add tests for kIfEverNonZero once Gauge and UpDownCounter support
// attributes.

TYPED_TEST(ScalarMetricImplReportingPolicyPerCombination,
           KUnconditionallyOverridesGlobalKIfCurrentlyNonZero) {
    ScalarMetricImpl<TypeParam, bool> impl(ReportingPolicy::kIfCurrentlyNonZero,
                                           {.name = "is_cool", .values = {true, false}});
    Counter<TypeParam, bool>& counter = impl;
    counter.setReportingPolicy({true}, ReportingPolicy::kUnconditionally);
    EXPECT_THAT(impl.values(),
                ElementsAre(IsAttributesAndValue(
                    ElementsAre(AttributeNameAndValue{.name = "is_cool", .value = true}), 0)));
}

#ifdef MONGO_CONFIG_OTEL
TYPED_TEST(ScalarMetricImplReportingPolicyPerCombination, MultiplePoliciesAreIndependent) {
    ScalarMetricImpl<TypeParam, bool, int64_t> impl({.name = "is_cool", .values = {true, false}},
                                                    {.name = "size", .values = {1, 2}});
    Counter<TypeParam, bool, int64_t>& counter = impl;
    counter.setReportingPolicy({true, 1}, ReportingPolicy::kUnconditionally);
    counter.setReportingPolicy({true, 2}, ReportingPolicy::kIfCurrentlyNonZero);
    counter.add(5, {true, 2});
    counter.add(3, {false, 2});
    impl.reset(nullptr);

    EXPECT_THAT(
        impl.values(),
        ElementsAre(IsAttributesAndValue(
            UnorderedElementsAre(AttributeNameAndValue{.name = "is_cool", .value = true},
                                 AttributeNameAndValue{.name = "size", .value = int64_t{1}}),
            0)));
}
#endif  // MONGO_CONFIG_OTEL

TYPED_TEST(ScalarMetricImplReportingPolicyPerCombination, ThrowsOnUndeclaredCombination) {
    ScalarMetricImpl<TypeParam, int64_t> impl({.name = "size", .values = {1, 2}});
    Counter<TypeParam, int64_t>& counter = impl;
    ASSERT_THROWS_CODE(counter.setReportingPolicy({3}, ReportingPolicy::kUnconditionally),
                       DBException,
                       ErrorCodes::BadValue);
}

///////////////////////////////////////////////////////////////////////////////
// Concurrency tests (TSAN coverage for all instrument types with attributes)
///////////////////////////////////////////////////////////////////////////////

template <typename T>
class ConcurrentTest : public testing::Test {};

using ConcurrentTypes = testing::Types<int64_t, double>;
TYPED_TEST_SUITE(ConcurrentTest, ConcurrentTypes);

// Exercises Counter, Gauge, UpDownCounter, MinGauge, and MaxGauge concurrently.
// Thread i writes to the slot {i%3, i%2==0}, covering all 6 combinations of {0,1,2}×{true,false}.
// After joining, minGauge must equal 0 and maxGauge must equal kIterationsPerThread - 1.
TYPED_TEST(ConcurrentTest, AllOperationsConcurrent) {
    ScalarMetricImpl<TypeParam, int32_t, bool> impl({.name = "bucket", .values = {0, 1, 2}},
                                                    {.name = "is_active", .values = {true, false}});
    // View this single metric as all possible types.
    Counter<TypeParam, int32_t, bool>& counter = impl;
    Gauge<TypeParam, int32_t, bool>& gauge = impl;
    UpDownCounter<TypeParam, int32_t, bool>& upDownCounter = impl;
    ObservableScalarMetric<TypeParam>& observableScalarMetric = impl;

    GaugeImpl<TypeParam> minGauge{std::numeric_limits<TypeParam>::max()};
    GaugeImpl<TypeParam> maxGauge{std::numeric_limits<TypeParam>::lowest()};

    constexpr int kNumThreads = 10;
    constexpr int kIterationsPerThread = 1000;

    std::vector<stdx::thread> threads;
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back(
            [&counter, &gauge, &upDownCounter, &observableScalarMetric, &minGauge, &maxGauge, i]() {
                int32_t bucket = i % 3;
                bool isActive = i % 2 == 0;
                for (int j = 0; j < kIterationsPerThread; ++j) {
                    // Both read and write to the metric, as each possible implementation.
                    observableScalarMetric.values();
                    counter.add(1, {bucket, isActive});
                    gauge.set(j, {bucket, isActive});
                    upDownCounter.add(1, {bucket, isActive});
                    minGauge.setIfLess(static_cast<TypeParam>(j));
                    maxGauge.setIfGreater(static_cast<TypeParam>(j));
                }
            });
    }

    for (stdx::thread& thread : threads) {
        thread.join();
    }

    EXPECT_THAT(minGauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    EXPECT_THAT(maxGauge.values(),
                ElementsAre(IsAttributesAndValue(IsEmpty(), kIterationsPerThread - 1)));
}

}  // namespace
}  // namespace mongo::otel::metrics
