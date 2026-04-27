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

#include "mongo/otel/metrics/metrics_histogram.h"

#include "mongo/unittest/unittest.h"

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/provider.h>
#endif  // MONGO_CONFIG_OTEL

namespace mongo::otel::metrics {

template <typename T>
std::unique_ptr<HistogramImpl<T>> createHistogram() {
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

template <typename T>
class HistogramImplTest : public testing::Test {};

using HistogramTypes = testing::Types<int64_t, double>;
TYPED_TEST_SUITE(HistogramImplTest, HistogramTypes);

TYPED_TEST(HistogramImplTest, Records) {
    std::unique_ptr<HistogramImpl<TypeParam>> histogram = createHistogram<TypeParam>();
    histogram->record(0);
    histogram->record(std::numeric_limits<TypeParam>::max());
    ASSERT_THROWS_CODE(histogram->record(-1), DBException, ErrorCodes::BadValue);
}

TYPED_TEST(HistogramImplTest, Serialization) {
    std::unique_ptr<HistogramImpl<TypeParam>> histogram = createHistogram<TypeParam>();
    const std::string key = "histogram_seconds";
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 0.0 << "count" << 0)));

    histogram->record(10);
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 10.0 << "count" << 1)));

    ASSERT_THROWS_CODE(histogram->record(-1), DBException, ErrorCodes::BadValue);
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 10.0 << "count" << 1)));
}

TEST(Int64HistogramImplTest, RejectsUint64Max) {
    std::unique_ptr<HistogramImpl<int64_t>> histogram = createHistogram<int64_t>();
    // Using two's complement, 0xFFFFFFFFFFFFFFFF interpreted as int64_t is -1.
    ASSERT_THROWS_CODE(
        histogram->record(std::numeric_limits<uint64_t>::max()), DBException, ErrorCodes::BadValue);
}

TEST(DoubleHistogramImplTest, RecordsFractionalValues) {
    std::unique_ptr<HistogramImpl<double>> histogram = createHistogram<double>();
    histogram->record(3.14);
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson("histogram"),
                      BSON("histogram" << BSON("average" << 3.14 << "count" << 1)));
}

template <typename T, typename... AttributeTs>
std::unique_ptr<HistogramImpl<T, AttributeTs...>> createHistogramWithDefs(
    const AttributeDefinition<AttributeTs>&... defs) {
#ifdef MONGO_CONFIG_OTEL
    return std::make_unique<HistogramImpl<T, AttributeTs...>>(
        *opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("test_meter"),
        "name",
        "description",
        "unit",
        boost::none,
        defs...);
#else
    return std::make_unique<HistogramImpl<T, AttributeTs...>>(defs...);
#endif  // MONGO_CONFIG_OTEL
}

TEST(HistogramImplWithAttributesTest, ThrowsOnDuplicateAttributeValues) {
    ASSERT_THROWS_CODE(createHistogramWithDefs<int64_t>(AttributeDefinition<bool>{
                           .name = "is_internal", .values = {true, true}}),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(HistogramImplWithAttributesTest, ThrowsOnEmptyAttributeValues) {
    ASSERT_THROWS_CODE(createHistogramWithDefs<int64_t>(
                           AttributeDefinition<StringData>{.name = "type", .values = {}}),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(HistogramImplWithAttributesTest, ThrowsOnDuplicateAttributeNames) {
    ASSERT_THROWS_CODE(
        (createHistogramWithDefs<int64_t>(
            AttributeDefinition<bool>{.name = "is_internal", .values = {true, false}},
            AttributeDefinition<StringData>{.name = "is_internal", .values = {"foo", "bar"}})),
        DBException,
        ErrorCodes::BadValue);
}

TEST(HistogramImplWithAttributesTest, ThrowsOnInvalidAttributes) {
    auto histogram = createHistogramWithDefs<int64_t>(
        AttributeDefinition<StringData>{.name = "type", .values = {"foo", "bar"}});
    histogram->record(10, {"foo"_sd});
    ASSERT_THROWS_CODE(histogram->record(10, {"x"_sd}), DBException, ErrorCodes::BadValue);
}

TEST(HistogramImplWithAttributesTest, StringDataAttributeValueIsCopied) {
    // If the source strings are destroyed after histogram creation, the histogram must have its own
    // copies and remain valid. Sanitizer builds will catch use-after-free if it does not.
    auto sourceValues = std::make_unique<std::vector<std::string>>(
        std::initializer_list<std::string>{"foo", "bar"});
    auto histogram = createHistogramWithDefs<int64_t>(AttributeDefinition<StringData>{
        .name = "type", .values = {(*sourceValues)[0], (*sourceValues)[1]}});
    sourceValues = nullptr;

    histogram->record(10, {"foo"_sd});
    histogram->record(20, {"bar"_sd});
}

TEST(HistogramImplWithAttributesTest, SpanAttributeValueIsCopied) {
    // If the source span data is destroyed after histogram creation, the histogram must have its
    // own copies and remain valid. Sanitizer builds will catch use-after-free if it does not.
    auto sourceIntData = std::make_unique<std::vector<std::vector<int32_t>>>(
        std::vector<std::vector<int32_t>>{{1, 2}, {3, 4}});
    auto histogram = createHistogramWithDefs<int64_t>(AttributeDefinition<std::span<int32_t>>{
        .name = "data",
        .values = {std::span<int32_t>((*sourceIntData)[0]),
                   std::span<int32_t>((*sourceIntData)[1])}});
    sourceIntData = nullptr;

    std::vector<int32_t> input{1, 2};
    histogram->record(10, {std::span<int32_t>(input)});
}

TEST(HistogramImplWithAttributesTest, RecordsWithSingleAttribute) {
    auto histogram = createHistogramWithDefs<int64_t>(
        AttributeDefinition<bool>{.name = "is_internal", .values = {true, false}});
    histogram->record(10, {true});
    histogram->record(20, {false});
    ASSERT_THROWS_CODE(histogram->record(-1, {true}), DBException, ErrorCodes::BadValue);
}

TEST(HistogramImplWithAttributesTest, SerializationWithSingleAttribute) {
    auto histogram = createHistogramWithDefs<int64_t>(
        AttributeDefinition<bool>{.name = "is_internal", .values = {true, false}});
    const std::string key = "histogram_seconds";
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 0.0 << "count" << 0)));

    histogram->record(10, {true});
    histogram->record(20, {false});
    // "average" is the exponential moving average of the values above.
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 12.0 << "count" << 2)));
}

TEST(HistogramImplWithAttributesTest, SerializationWithMultipleAttributes) {
    auto histogram = createHistogramWithDefs<int64_t>(
        AttributeDefinition<bool>{.name = "is_internal", .values = {true, false}},
        AttributeDefinition<int64_t>{.name = "priority", .values = {1, 2}});
    const std::string key = "histogram_seconds";
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 0.0 << "count" << 0)));

    histogram->record(10, {true, int64_t{1}});
    histogram->record(10, {true, int64_t{2}});
    histogram->record(10, {false, int64_t{1}});
    histogram->record(20, {false, int64_t{2}});
    // "average" is the exponential moving average of the values above.
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 12.0 << "count" << 4)));
}

}  // namespace mongo::otel::metrics
