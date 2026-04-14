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

#include "mongo/otel/metrics/metrics_attributes.h"

#include "mongo/otel/metrics/metrics_attributes_test_utils.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <tuple>
#include <vector>

namespace mongo::otel::metrics {

using testing::ElementsAre;
using testing::IsEmpty;
using testing::Pair;
using testing::UnorderedElementsAre;

TEST(SafeMakeAttributeTuplesTest, NoArgs) {
    std::vector<std::tuple<>> result = safeMakeAttributeTuples();
    EXPECT_THAT(result, ElementsAre(std::tuple<>{}));
}

TEST(SafeMakeAttributeTuplesTest, SingleVector) {
    std::vector<std::tuple<int>> result = safeMakeAttributeTuples(std::vector{1, 2, 3});
    EXPECT_THAT(result,
                UnorderedElementsAre(std::make_tuple(1), std::make_tuple(2), std::make_tuple(3)));
}

TEST(SafeMakeAttributeTuplesTest, TwoVectors) {
    auto result =
        safeMakeAttributeTuples(std::vector<StringData>{"a"_sd, "b"_sd, "c"_sd}, std::vector{1, 2});
    EXPECT_THAT(result,
                UnorderedElementsAre(std::make_tuple("a"_sd, 1),
                                     std::make_tuple("a"_sd, 2),
                                     std::make_tuple("b"_sd, 1),
                                     std::make_tuple("b"_sd, 2),
                                     std::make_tuple("c"_sd, 1),
                                     std::make_tuple("c"_sd, 2)));
}

TEST(SafeMakeAttributeTuplesTest, ThreeVectors) {
    auto result = safeMakeAttributeTuples(
        std::vector<StringData>{"a"_sd, "b"_sd}, std::vector{1, 2}, std::vector{true, false});
    EXPECT_THAT(result,
                UnorderedElementsAre(std::make_tuple("a"_sd, 1, true),
                                     std::make_tuple("a"_sd, 1, false),
                                     std::make_tuple("a"_sd, 2, true),
                                     std::make_tuple("a"_sd, 2, false),
                                     std::make_tuple("b"_sd, 1, true),
                                     std::make_tuple("b"_sd, 1, false),
                                     std::make_tuple("b"_sd, 2, true),
                                     std::make_tuple("b"_sd, 2, false)));
}

TEST(SafeMakeAttributeTuplesTest, VectorsWithSpans) {
    std::vector<int> ints1{1, 2};
    std::vector<int> ints2{3, 4};
    auto result = safeMakeAttributeTuples(std::vector<StringData>{"a"_sd, "b"_sd, "c"_sd},
                                          std::vector<std::span<int>>{ints1, ints2});
    EXPECT_THAT(
        result,
        UnorderedElementsAre(IsAttributesTuple(std::make_tuple("a"_sd, std::span<int>(ints1))),
                             IsAttributesTuple(std::make_tuple("a"_sd, std::span<int>(ints2))),
                             IsAttributesTuple(std::make_tuple("b"_sd, std::span<int>(ints1))),
                             IsAttributesTuple(std::make_tuple("b"_sd, std::span<int>(ints2))),
                             IsAttributesTuple(std::make_tuple("c"_sd, std::span<int>(ints1))),
                             IsAttributesTuple(std::make_tuple("c"_sd, std::span<int>(ints2)))));
}

TEST(SafeMakeAttributeTuplesTest, EmptyFirstVector) {
    auto result = safeMakeAttributeTuples(std::vector<int>{}, std::vector{1, 2});
    EXPECT_THAT(result, IsEmpty());
}

TEST(SafeMakeAttributeTuplesTest, EmptySecondVector) {
    auto result = safeMakeAttributeTuples(std::vector{1, 2}, std::vector<int>{});
    EXPECT_THAT(result, IsEmpty());
}

TEST(SafeMakeAttributeTuplesTest, ThrowsExceptionWhenAttributesHaveDuplicates) {
    ASSERT_THROWS_CODE(
        safeMakeAttributeTuples(std::vector{true, true}), DBException, ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(safeMakeAttributeTuples(std::vector{1, 2}, std::vector{1, 2, 3, 2}),
                       DBException,
                       ErrorCodes::BadValue);
    std::vector<int32_t> ints{1, 2, 3, 2};
    ASSERT_THROWS_CODE(
        safeMakeAttributeTuples(std::vector{std::span<int32_t>(ints), std::span<int32_t>(ints)}),
        DBException,
        ErrorCodes::BadValue);
}

TEST(SafeMakeAttributeTuplesTest, ThrowsExceptionWhenResultIsTooBig) {
    // 10 * 10 * 10 * 2 = 2000
    // 2000 > kMaxAttributeCombinationsPerMetric (1000)
    ASSERT_THROWS_CODE(safeMakeAttributeTuples(std::vector{1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
                                               std::vector{1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
                                               std::vector{1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
                                               std::vector{true, false}),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(ContainsDuplicates, ReturnsFalseOnEmpty) {
    EXPECT_FALSE(containsDuplicates({}));
}

TEST(ContainsDuplicates, ReturnsFalseIfNoDuplicates) {
    EXPECT_FALSE(containsDuplicates(std::vector<std::string>{"foo", "bar", "baz"}));
}

TEST(ContainsDuplicates, ReturnsTrueIfDuplicates) {
    EXPECT_TRUE(containsDuplicates(std::vector<std::string>{"foo", "foo"}));
    EXPECT_TRUE(
        containsDuplicates(std::vector<std::string>{"foo", "bar", "baz", "bing", "bar", "bif"}));
}

TEST(AttributeValuesEqualTest, ScalarTypes) {
    EXPECT_TRUE(attributeValuesEqual(true, true));
    EXPECT_TRUE(attributeValuesEqual(false, false));
    EXPECT_FALSE(attributeValuesEqual(true, false));

    EXPECT_TRUE(attributeValuesEqual(int32_t{42}, int32_t{42}));
    EXPECT_FALSE(attributeValuesEqual(int32_t{42}, int32_t{43}));

    EXPECT_TRUE(attributeValuesEqual(int64_t{100}, int64_t{100}));
    EXPECT_FALSE(attributeValuesEqual(int64_t{100}, int64_t{101}));

    EXPECT_TRUE(attributeValuesEqual(1.5, 1.5));
    EXPECT_FALSE(attributeValuesEqual(1.5, 1.6));

    EXPECT_TRUE(attributeValuesEqual("hello"_sd, "hello"_sd));
    EXPECT_FALSE(attributeValuesEqual("hello"_sd, "world"_sd));
}

TEST(AttributeValuesEqualTest, SpanTypes) {
    std::array<bool, 2> boolArr1{true, false};
    std::array<bool, 2> boolArr2{true, false};
    std::array<bool, 2> boolArr3{false, true};
    EXPECT_TRUE(attributeValuesEqual(std::span<bool>(boolArr1), std::span<bool>(boolArr2)));
    EXPECT_FALSE(attributeValuesEqual(std::span<bool>(boolArr1), std::span<bool>(boolArr3)));

    std::vector<int32_t> i32Vec1{1, 2, 3};
    std::vector<int32_t> i32Vec2{1, 2, 3};
    std::vector<int32_t> i32Vec3{1, 2, 4};
    EXPECT_TRUE(attributeValuesEqual(std::span<int32_t>(i32Vec1), std::span<int32_t>(i32Vec2)));
    EXPECT_FALSE(attributeValuesEqual(std::span<int32_t>(i32Vec1), std::span<int32_t>(i32Vec3)));

    std::vector<int64_t> i64Vec1{10, 20};
    std::vector<int64_t> i64Vec2{10, 20};
    std::vector<int64_t> i64Vec3{10, 21};
    EXPECT_TRUE(attributeValuesEqual(std::span<int64_t>(i64Vec1), std::span<int64_t>(i64Vec2)));
    EXPECT_FALSE(attributeValuesEqual(std::span<int64_t>(i64Vec1), std::span<int64_t>(i64Vec3)));

    std::vector<double> dblVec1{1.1, 2.2};
    std::vector<double> dblVec2{1.1, 2.2};
    std::vector<double> dblVec3{1.1, 2.3};
    EXPECT_TRUE(attributeValuesEqual(std::span<double>(dblVec1), std::span<double>(dblVec2)));
    EXPECT_FALSE(attributeValuesEqual(std::span<double>(dblVec1), std::span<double>(dblVec3)));

    std::string s1 = "foo", s2 = "bar";
    std::string s3 = "foo", s4 = "bar";
    std::string s5 = "foo", s6 = "baz";
    std::vector<StringData> sdVec1{s1, s2};
    std::vector<StringData> sdVec2{s3, s4};
    std::vector<StringData> sdVec3{s5, s6};
    EXPECT_TRUE(attributeValuesEqual(std::span<StringData>(sdVec1), std::span<StringData>(sdVec2)));
    EXPECT_FALSE(
        attributeValuesEqual(std::span<StringData>(sdVec1), std::span<StringData>(sdVec3)));
}

TEST(AttributeValuesEqualTest, SpansDifferentLengths) {
    std::vector<int32_t> longVec{1, 2, 3};
    std::vector<int32_t> shortVec{1, 2};
    EXPECT_FALSE(attributeValuesEqual(std::span<int32_t>(longVec), std::span<int32_t>(shortVec)));
}

TEST(AttributeValuesEqualTest, EmptySpans) {
    std::vector<int32_t> empty1, empty2;
    EXPECT_TRUE(attributeValuesEqual(std::span<int32_t>(empty1), std::span<int32_t>(empty2)));
}

TEST(AttributesMapTest, ScalarKey) {
    AttributesMap<std::tuple<bool>, int> map;
    map[{true}] = 1;
    map[{false}] = 2;

    EXPECT_EQ(map.at({true}), 1);
    EXPECT_EQ(map.at({false}), 2);
    EXPECT_EQ(map.size(), 2u);
}

TEST(AttributesMapTest, MultipleScalarKeys) {
    AttributesMap<std::tuple<bool, int64_t>, int> map;
    map[{true, int64_t{1}}] = 10;
    map[{false, int64_t{2}}] = 20;
    map[{true, int64_t{2}}] = 30;

    EXPECT_THAT(map,
                UnorderedElementsAre(Pair(std::make_tuple(true, int64_t{1}), 10),
                                     Pair(std::make_tuple(false, int64_t{2}), 20),
                                     Pair(std::make_tuple(true, int64_t{2}), 30)));
}

TEST(AttributesMapTest, SpanKeyContentEqualityNotPointerEquality) {
    // Two spans with identical content but different backing storage must map to the same key.
    std::vector<int32_t> data1{1, 2, 3};
    std::vector<int32_t> data2{1, 2, 3};

    AttributesMap<std::tuple<std::span<int32_t>>, int> map;
    map[{std::span<int32_t>(data1)}] = 5;
    map[{std::span<int32_t>(data2)}] += 6;

    // Lookup using a different vector with the same content.
    EXPECT_THAT(map,
                UnorderedElementsAre(
                    Pair(IsAttributesTuple(std::make_tuple(std::span<int32_t>(data2))), 11)));
}

TEST(AttributesMapTest, SpanKeyDistinctContent) {
    std::vector<int32_t> data1{1, 2, 3};
    std::vector<int32_t> data2{4, 5, 6};

    AttributesMap<std::tuple<std::span<int32_t>>, int> map;
    map[{std::span<int32_t>(data1)}] = 1;
    map[{std::span<int32_t>(data2)}] = 2;

    EXPECT_THAT(map,
                UnorderedElementsAre(
                    Pair(IsAttributesTuple(std::make_tuple(std::span<int32_t>(data1))), 1),
                    Pair(IsAttributesTuple(std::make_tuple(std::span<int32_t>(data2))), 2)));
}

TEST(AttributesMapTest, StringDataKey) {
    AttributesMap<std::tuple<StringData>, int> map;
    map[{"hello"_sd}] = 1;
    map[{"world"_sd}] = 2;

    EXPECT_THAT(map,
                UnorderedElementsAre(Pair(std::make_tuple("hello"_sd), 1),
                                     Pair(std::make_tuple("world"_sd), 2)));
}

TEST(AttributesMapTest, ScalarAndSpanKeys) {
    std::vector<int32_t> data1{1, 2, 3};
    std::vector<int32_t> data2{4, 5, 6};

    AttributesMap<std::tuple<bool, std::span<int32_t>>, int> map;
    map[{true, data1}] = 1;
    map[{true, data1}] += 5;
    map[{true, data2}] = 99;
    map[{false, data1}] = 100;

    EXPECT_THAT(
        map,
        UnorderedElementsAre(
            Pair(IsAttributesTuple(std::make_tuple(true, std::span<int32_t>(data1))), 6),
            Pair(IsAttributesTuple(std::make_tuple(true, std::span<int32_t>(data2))), 99),
            Pair(IsAttributesTuple(std::make_tuple(false, std::span<int32_t>(data1))), 100)));
}

}  // namespace mongo::otel::metrics
