// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/field_values.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace mongo::stats {
namespace {

using sbe::value::TagValueView;
using sbe::value::TypeTags;

std::vector<FieldPath> makePaths(std::initializer_list<std::string_view> paths) {
    return {paths.begin(), paths.end()};
}

BSONArray makeArrayValues(int count) {
    BSONArrayBuilder values;
    for (int i = 0; i < count; ++i) {
        values.append(BSON("x" << i));
    }
    return values.arr();
}

int32_t asInt32(TagValueView value) {
    ASSERT_EQ(value.tag, TypeTags::NumberInt32);
    return sbe::value::bitcastTo<int32_t>(value.value);
}

// Tuples of int32 values, e.g. {{1, 3}, {1, 4}}, for asserting on the whole cross product.
std::vector<std::vector<int32_t>> extractInt32Tuples(
    const BSONObj& doc, std::initializer_list<std::string_view> paths) {
    std::vector<std::vector<int32_t>> result;
    const auto [tuples, hasNonScalar] = extractFieldValues(doc, makePaths(paths));
    for (const auto& tuple : tuples) {
        auto& values = result.emplace_back();
        for (const auto& value : tuple) {
            values.push_back(asInt32(value));
        }
    }
    return result;
}

TEST(FieldValuesTest, ReturnsValuesInRequestedPathOrder) {
    ASSERT_EQ(extractInt32Tuples(fromjson("{a: 1, b: 2}"), {"b", "a"}),
              (std::vector<std::vector<int32_t>>{{2, 1}}));
}

TEST(FieldValuesTest, RetainsNonScalarFlagFromEarlierPath) {
    const auto doc = fromjson("{a: [{b: 1}], c: 9}");
    const auto [tuples, hasNonScalar] = extractFieldValues(doc, makePaths({"a.b", "c"}));
    ASSERT_TRUE(hasNonScalar);
}

TEST(FieldValuesTest, ProducesCrossProductOfRequestedPaths) {
    const auto doc = fromjson("{a: [{x: 1}, {x: 2}], b: [{y: 3}, {y: 4}], c: 9}");
    ASSERT_EQ(extractInt32Tuples(doc, {"a.x", "c", "b.y"}),
              (std::vector<std::vector<int32_t>>{{1, 9, 3}, {1, 9, 4}, {2, 9, 3}, {2, 9, 4}}));
}

TEST(FieldValuesTest, AllowsCrossProductAtPerDocumentLimit) {
    const auto doc = BSON("a" << makeArrayValues(1000) << "b" << makeArrayValues(100));
    const auto [tuples, hasNonScalar] = extractFieldValues(doc, makePaths({"a.x", "b.x"}));
    ASSERT_EQ(tuples.size(), 100'000);
}

TEST(FieldValuesTest, RejectsCrossProductAbovePerDocumentLimit) {
    const auto doc = BSON("a" << makeArrayValues(1000) << "b" << makeArrayValues(101));
    ASSERT_THROWS_CODE(extractFieldValues(doc, makePaths({"a.x", "b.x"})),
                       DBException,
                       ErrorCodes::ExceededMemoryLimit);
}

TEST(FieldValuesTest, FillsMissingPathInEveryTupleWithNothing) {
    const auto doc = fromjson("{a: [{b: 1}, {b: 2}]}");
    const auto [tuples, hasNonScalar] = extractFieldValues(doc, makePaths({"a.b", "absent"}));
    ASSERT_EQ(tuples.size(), 2);
    for (const auto& tuple : tuples) {
        ASSERT_EQ(tuple.size(), 2);
        ASSERT_EQ(tuple[1].tag, TypeTags::Nothing);
    }
}

}  // namespace
}  // namespace mongo::stats
