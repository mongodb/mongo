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
#include <string_view>

#include <opentelemetry/metrics/provider.h>
#endif  // MONGO_CONFIG_OTEL

namespace mongo::otel::metrics {
using namespace std::literals::string_view_literals;

namespace {
// Builds the expected BSON for a histogram using kDefaultBucketBoundaries.
// bucketCounts must have kDefaultBucketBoundaries.size() + 1 = 16 elements.
BSONObj makeDefaultBucketsBson(const std::string& key, const std::vector<int64_t>& bucketCounts) {
    invariant(bucketCounts.size() == kDefaultBucketBoundaries.size() + 1);
    const auto& bounds = kDefaultBucketBoundaries;
    int64_t totalCount = 0;
    BSONObjBuilder outer;
    BSONObjBuilder inner{outer.subobjStart(key)};
    for (size_t i = 0; i < bucketCounts.size(); ++i) {
        totalCount += bucketCounts[i];
        std::string bucketKey = fmt::format(
            "{}{}, {})",
            i == 0 ? "(" : "[",
            i == 0 ? std::string("-inf") : fmt::format("{}", bounds[i - 1]),
            i + 1 == bucketCounts.size() ? std::string("inf") : fmt::format("{}", bounds[i]));
        BSONObjBuilder{inner.subobjStart(bucketKey)}.append("count", bucketCounts[i]);
    }
    inner.append("totalCount", totalCount);
    inner.doneFast();
    return outer.obj();
}
}  // namespace

template <typename T>
std::unique_ptr<HistogramImpl<T>> createHistogramAverageFormatDefaultBoundaries() {
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

template <typename T>
std::unique_ptr<HistogramImpl<T>> createHistogramAverageFormatExplicitBoundaries(
    std::vector<double> boundaries) {
#ifdef MONGO_CONFIG_OTEL
    return std::make_unique<HistogramImpl<T>>(
        *opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("test_meter"),
        "name",
        "description",
        "unit",
        HistogramSerializationFormat::kAverage,
        boundaries);
#else
    return std::make_unique<HistogramImpl<T>>(HistogramSerializationFormat::kAverage, boundaries);
#endif  // MONGO_CONFIG_OTEL
}

template <typename T>
std::unique_ptr<HistogramImpl<T>> createHistogramBucketCountsFormatDefaultBoundaries() {
#ifdef MONGO_CONFIG_OTEL
    return std::make_unique<HistogramImpl<T>>(
        *opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("test_meter"),
        "name",
        "description",
        "unit",
        HistogramSerializationFormat::kBucketCounts);
#else
    return std::make_unique<HistogramImpl<T>>(HistogramSerializationFormat::kBucketCounts);
#endif  // MONGO_CONFIG_OTEL
}

template <typename T>
std::unique_ptr<HistogramImpl<T>> createHistogramBucketCountsFormatExplicitBoundaries(
    std::vector<double> boundaries) {
#ifdef MONGO_CONFIG_OTEL
    return std::make_unique<HistogramImpl<T>>(
        *opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("test_meter"),
        "name",
        "description",
        "unit",
        HistogramSerializationFormat::kBucketCounts,
        boundaries);
#else
    return std::make_unique<HistogramImpl<T>>(HistogramSerializationFormat::kBucketCounts,
                                              boundaries);
#endif  // MONGO_CONFIG_OTEL
}

template <typename T>
std::unique_ptr<HistogramImpl<T>> createHistogram() {
    return createHistogramAverageFormatDefaultBoundaries<T>();
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


TYPED_TEST(HistogramImplTest, SerializationAverageFormatWithDefaultBoundaries) {
    std::unique_ptr<HistogramImpl<TypeParam>> histogram =
        createHistogramAverageFormatDefaultBoundaries<TypeParam>();
    const std::string key = "histogram_seconds";
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 0.0 << "totalCount" << 0LL)));

    histogram->record(10);
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 10.0 << "totalCount" << 1LL)));

    // A failed record does not corrupt the serialized state.
    ASSERT_THROWS_CODE(histogram->record(-1), DBException, ErrorCodes::BadValue);
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 10.0 << "totalCount" << 1LL)));
}

TYPED_TEST(HistogramImplTest, SerializationAverageFormatWithExplicitBoundaries) {
    // Explicit boundaries affect OTel aggregation but not serverStatus output. Expect
    // average+totalCount format regardless of whether explicit boundaries are set.
    std::unique_ptr<HistogramImpl<TypeParam>> histogram =
        createHistogramAverageFormatExplicitBoundaries<TypeParam>({2, 4});
    const std::string key = "histogram_seconds";

    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 0.0 << "totalCount" << 0LL)));

    histogram->record(3);
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 3.0 << "totalCount" << 1LL)));
}

TYPED_TEST(HistogramImplTest, SerializationBucketCountsFormatWithExplicitBoundaries) {
    std::unique_ptr<HistogramImpl<TypeParam>> histogram =
        createHistogramBucketCountsFormatExplicitBoundaries<TypeParam>({2, 4});
    const std::string key = "histogram_seconds";

    ASSERT_BSONOBJ_EQ(
        histogram->serializeToBson(key),
        BSON(key << BSON("(-inf, 2)" << BSON("count" << 0LL) << "[2, 4)" << BSON("count" << 0LL)
                                     << "[4, inf)" << BSON("count" << 0LL) << "totalCount"
                                     << 0LL)));

    histogram->record(1);  // (-inf, 2)
    histogram->record(2);  // [2, 4) — value at boundary goes into [boundary, ...)
    histogram->record(3);  // [2, 4)
    histogram->record(5);  // [4, inf)

    ASSERT_BSONOBJ_EQ(
        histogram->serializeToBson(key),
        BSON(key << BSON("(-inf, 2)" << BSON("count" << 1LL) << "[2, 4)" << BSON("count" << 2LL)
                                     << "[4, inf)" << BSON("count" << 1LL) << "totalCount"
                                     << 4LL)));
}

TYPED_TEST(HistogramImplTest, SerializationBucketCountsFormatWithDefaultBoundaries) {
    std::unique_ptr<HistogramImpl<TypeParam>> histogram =
        createHistogramBucketCountsFormatDefaultBoundaries<TypeParam>();
    const std::string key = "histogram_seconds";

    ASSERT_BSONOBJ_EQ(
        histogram->serializeToBson(key),
        makeDefaultBucketsBson(key, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));

    histogram->record(10);  // goes into [10, 25) bucket (index 3)
    ASSERT_BSONOBJ_EQ(
        histogram->serializeToBson(key),
        makeDefaultBucketsBson(key, {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
}

#ifdef MONGO_CONFIG_OTEL
TYPED_TEST(HistogramImplTest, ResetAverageFormat) {
    auto histogram = createHistogramAverageFormatDefaultBoundaries<TypeParam>();
    const std::string key = "histogram_seconds";
    histogram->record(10);

    auto meter = opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("test_meter");
    histogram->reset(meter.get());

    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 0.0 << "totalCount" << 0LL)));
}

TYPED_TEST(HistogramImplTest, ResetBucketCountsFormat) {
    auto histogram = createHistogramBucketCountsFormatExplicitBoundaries<TypeParam>({2, 4});
    const std::string key = "histogram_seconds";
    histogram->record(1);  // (-inf, 2)
    histogram->record(3);  // [2, 4)
    histogram->record(5);  // [4, inf)

    auto meter = opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("test_meter");
    histogram->reset(meter.get());

    ASSERT_BSONOBJ_EQ(
        histogram->serializeToBson(key),
        BSON(key << BSON("(-inf, 2)" << BSON("count" << 0LL) << "[2, 4)" << BSON("count" << 0LL)
                                     << "[4, inf)" << BSON("count" << 0LL) << "totalCount"
                                     << 0LL)));
}
#endif  // MONGO_CONFIG_OTEL

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
                      BSON("histogram" << BSON("average" << 3.14 << "totalCount" << 1LL)));
}

template <typename T, typename... AttributeTs>
std::unique_ptr<HistogramImpl<T, AttributeTs...>>
createHistogramAverageFormatDefaultBoundariesWithDefs(
    const AttributeDefinition<AttributeTs>&... defs) {
#ifdef MONGO_CONFIG_OTEL
    return std::make_unique<HistogramImpl<T, AttributeTs...>>(
        *opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("test_meter"),
        "name",
        "description",
        "unit",
        HistogramSerializationFormat::kAverage,
        /*explicitBucketBoundaries=*/boost::none,
        defs...);
#else
    return std::make_unique<HistogramImpl<T, AttributeTs...>>(
        HistogramSerializationFormat::kAverage, boost::none, defs...);
#endif  // MONGO_CONFIG_OTEL
}

template <typename T, typename... AttributeTs>
std::unique_ptr<HistogramImpl<T, AttributeTs...>>
createHistogramBucketCountsFormatExplicitBoundariesWithDefs(
    std::vector<double> boundaries, const AttributeDefinition<AttributeTs>&... defs) {
#ifdef MONGO_CONFIG_OTEL
    return std::make_unique<HistogramImpl<T, AttributeTs...>>(
        *opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("test_meter"),
        "name",
        "description",
        "unit",
        HistogramSerializationFormat::kBucketCounts,
        boundaries,
        defs...);
#else
    return std::make_unique<HistogramImpl<T, AttributeTs...>>(
        HistogramSerializationFormat::kBucketCounts, boundaries, defs...);
#endif  // MONGO_CONFIG_OTEL
}

template <typename T, typename... AttributeTs>
std::unique_ptr<HistogramImpl<T, AttributeTs...>> createHistogramWithDefs(
    const AttributeDefinition<AttributeTs>&... defs) {
    return createHistogramAverageFormatDefaultBoundariesWithDefs<T>(defs...);
}

TEST(HistogramImplWithAttributesTest, ThrowsOnDuplicateAttributeValues) {
    ASSERT_THROWS_CODE(createHistogramWithDefs<int64_t>(AttributeDefinition<bool>{
                           .name = "is_internal", .values = {true, true}}),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(HistogramImplWithAttributesTest, ThrowsOnEmptyAttributeValues) {
    ASSERT_THROWS_CODE(createHistogramWithDefs<int64_t>(
                           AttributeDefinition<std::string_view>{.name = "type", .values = {}}),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(HistogramImplWithAttributesTest, ThrowsOnDuplicateAttributeNames) {
    ASSERT_THROWS_CODE(
        (createHistogramWithDefs<int64_t>(
            AttributeDefinition<bool>{.name = "is_internal", .values = {true, false}},
            AttributeDefinition<std::string_view>{.name = "is_internal",
                                                  .values = {"foo", "bar"}})),
        DBException,
        ErrorCodes::BadValue);
}

TEST(HistogramImplWithAttributesTest, ThrowsOnInvalidAttributes) {
    auto histogram = createHistogramWithDefs<int64_t>(
        AttributeDefinition<std::string_view>{.name = "type", .values = {"foo", "bar"}});
    histogram->record(10, {"foo"sv});
    ASSERT_THROWS_CODE(histogram->record(10, {"x"sv}), DBException, ErrorCodes::BadValue);
}

TEST(HistogramImplWithAttributesTest, StringDataAttributeValueIsCopied) {
    // If the source strings are destroyed after histogram creation, the histogram must have its own
    // copies and remain valid. Sanitizer builds will catch use-after-free if it does not.
    auto sourceValues = std::make_unique<std::vector<std::string>>(
        std::initializer_list<std::string>{"foo", "bar"});
    auto histogram = createHistogramWithDefs<int64_t>(AttributeDefinition<std::string_view>{
        .name = "type", .values = {(*sourceValues)[0], (*sourceValues)[1]}});
    sourceValues = nullptr;

    histogram->record(10, {"foo"sv});
    histogram->record(20, {"bar"sv});
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

TEST(HistogramImplWithAttributesTest, SerializationWithSingleAttributeAverageFormat) {
    auto histogram = createHistogramAverageFormatDefaultBoundariesWithDefs<int64_t>(
        AttributeDefinition<bool>{.name = "is_internal", .values = {true, false}});
    const std::string key = "histogram_seconds";
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 0.0 << "totalCount" << 0LL)));

    histogram->record(10, {true});
    histogram->record(20, {false});
    // "average" is the exponential moving average of the values above.
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 12.0 << "totalCount" << 2LL)));
}

TEST(HistogramImplWithAttributesTest, SerializationWithSingleAttributeBucketCountsFormat) {
    auto histogram = createHistogramBucketCountsFormatExplicitBoundariesWithDefs<int64_t>(
        {2, 4}, AttributeDefinition<bool>{.name = "is_internal", .values = {true, false}});
    const std::string key = "histogram_seconds";

    histogram->record(1, {true});   // (-inf, 2)
    histogram->record(3, {false});  // [2, 4)
    histogram->record(5, {true});   // [4, inf)

    ASSERT_BSONOBJ_EQ(
        histogram->serializeToBson(key),
        BSON(key << BSON("(-inf, 2)" << BSON("count" << 1LL) << "[2, 4)" << BSON("count" << 1LL)
                                     << "[4, inf)" << BSON("count" << 1LL) << "totalCount"
                                     << 3LL)));
}

TEST(HistogramImplWithAttributesTest, SerializationWithMultipleAttributesAverageFormat) {
    auto histogram = createHistogramAverageFormatDefaultBoundariesWithDefs<int64_t>(
        AttributeDefinition<bool>{.name = "is_internal", .values = {true, false}},
        AttributeDefinition<int64_t>{.name = "priority", .values = {1, 2}});
    const std::string key = "histogram_seconds";
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 0.0 << "totalCount" << 0LL)));

    histogram->record(10, {true, int64_t{1}});
    histogram->record(10, {true, int64_t{2}});
    histogram->record(10, {false, int64_t{1}});
    histogram->record(20, {false, int64_t{2}});
    // "average" is the exponential moving average of the values above, aggregated across all
    // attribute combinations.
    ASSERT_BSONOBJ_EQ(histogram->serializeToBson(key),
                      BSON(key << BSON("average" << 12.0 << "totalCount" << 4LL)));
}

TEST(HistogramImplWithAttributesTest, SerializationWithMultipleAttributesBucketCountsFormat) {
    auto histogram = createHistogramBucketCountsFormatExplicitBoundariesWithDefs<int64_t>(
        {2, 4},
        AttributeDefinition<bool>{.name = "is_internal", .values = {true, false}},
        AttributeDefinition<int64_t>{.name = "priority", .values = {1, 2}});
    const std::string key = "histogram_seconds";

    histogram->record(1, {true, int64_t{1}});   // (-inf, 2)
    histogram->record(3, {true, int64_t{2}});   // [2, 4)
    histogram->record(5, {false, int64_t{1}});  // [4, inf)
    histogram->record(3, {false, int64_t{2}});  // [2, 4)

    // Bucket counts are aggregated across all attribute combinations.
    ASSERT_BSONOBJ_EQ(
        histogram->serializeToBson(key),
        BSON(key << BSON("(-inf, 2)" << BSON("count" << 1LL) << "[2, 4)" << BSON("count" << 2LL)
                                     << "[4, inf)" << BSON("count" << 1LL) << "totalCount"
                                     << 4LL)));
}

}  // namespace mongo::otel::metrics
