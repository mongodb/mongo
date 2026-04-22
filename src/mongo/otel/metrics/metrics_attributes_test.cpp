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
#include <typeindex>
#include <vector>

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/nostd/string_view.h>
#include <opentelemetry/nostd/variant.h>
#endif

namespace mongo::otel::metrics {
namespace {

using testing::_;
using testing::ElementsAre;
using testing::FieldsAre;
using testing::IsEmpty;
using testing::Pair;
using testing::Pointee;
using testing::UnorderedElementsAre;
using testing::VariantWith;

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

TEST(MakeOwnedAttributeValueListsTest, ScalarAttribute) {
    OwnedAttributeValueLists<int32_t> owned = makeOwnedAttributeValueLists(
        AttributeDefinition<int32_t>{.name = "n", .values = {1, 2, 3}});
    EXPECT_THAT(owned.lists, FieldsAre(UnorderedElementsAre(Pointee(1), Pointee(2), Pointee(3))));
}

TEST(MakeOwnedAttributeValueListsTest, StringDataAttributeOwned) {
    // If source strings are destroyed, owned must have its own copies.
    auto source = std::make_unique<std::vector<std::string>>(
        std::initializer_list<std::string>{"foo", "bar"});
    OwnedAttributeValueLists<StringData> owned = makeOwnedAttributeValueLists(
        AttributeDefinition<StringData>{.name = "s", .values = {(*source)[0], (*source)[1]}});
    source = nullptr;  // Sanitizers catch use-after-free if copies weren't made.
    EXPECT_THAT(owned.lists, FieldsAre(UnorderedElementsAre(Pointee("foo"_sd), Pointee("bar"_sd))));
}

TEST(MakeOwnedAttributeValueListsTest, SpanAttributeOwned) {
    // If source span data is destroyed, owned must have its own copies.
    auto source = std::make_unique<std::vector<std::vector<int32_t>>>(
        std::initializer_list<std::vector<int32_t>>{{1, 2}, {3, 4}});
    OwnedAttributeValueLists<std::span<int32_t>> owned =
        makeOwnedAttributeValueLists(AttributeDefinition<std::span<int32_t>>{
            .name = "data",
            .values = {std::span<int32_t>((*source)[0]), std::span<int32_t>((*source)[1])}});
    source = nullptr;
    EXPECT_THAT(
        owned.lists,
        FieldsAre(UnorderedElementsAre(Pointee(ElementsAre(1, 2)), Pointee(ElementsAre(3, 4)))));
}

TEST(MakeOwnedAttributeValueListsTest, MultipleAttributeTypes) {
    OwnedAttributeValueLists<StringData, bool> owned = makeOwnedAttributeValueLists(
        AttributeDefinition<StringData>{.name = "s", .values = {"a"_sd, "b"_sd}},
        AttributeDefinition<bool>{.name = "flag", .values = {true, false}});
    EXPECT_THAT(owned.lists,
                FieldsAre(UnorderedElementsAre(Pointee("a"_sd), Pointee("b"_sd)),
                          UnorderedElementsAre(Pointee(true), Pointee(false))));
}

TEST(SafeMakeAttributeTuplesOwnedTest, SpanOfBoolAttribute) {
    // std::vector<bool> is bit-packed and lacks data(), so span<bool> needs a separate owned type.
    // Source storage must be contiguous (array, not vector<bool>) to form span<bool>.
    std::array<bool, 2> bools1{true, false};
    std::array<bool, 2> bools2{false, true};
    OwnedAttributeValueLists<std::span<bool>> owned =
        makeOwnedAttributeValueLists(AttributeDefinition<std::span<bool>>{
            .name = "flags", .values = {std::span<bool>(bools1), std::span<bool>(bools2)}});
    EXPECT_THAT(safeMakeAttributeTuples(owned),
                UnorderedElementsAre(IsAttributesTuple(std::make_tuple(std::span<bool>{bools1})),
                                     IsAttributesTuple(std::make_tuple(std::span<bool>{bools2}))));
}

TEST(SafeMakeAttributeTuplesOwnedTest, NoAttributes) {
    OwnedAttributeValueLists<> owned = makeOwnedAttributeValueLists();
    EXPECT_THAT(safeMakeAttributeTuples(owned), ElementsAre(std::tuple<>{}));
}

TEST(SafeMakeAttributeTuplesOwnedTest, SingleAttribute) {
    OwnedAttributeValueLists<int32_t> owned = makeOwnedAttributeValueLists(
        AttributeDefinition<int32_t>{.name = "n", .values = {1, 2, 3}});
    EXPECT_THAT(safeMakeAttributeTuples(owned),
                UnorderedElementsAre(std::make_tuple(int32_t{1}),
                                     std::make_tuple(int32_t{2}),
                                     std::make_tuple(int32_t{3})));
}

TEST(SafeMakeAttributeTuplesOwnedTest, MultipleAttributes) {
    std::array<bool, 2> boolArr1{true, false};
    std::array<bool, 2> boolArr2{false, true};
    OwnedAttributeValueLists<StringData, std::span<bool>> owned = makeOwnedAttributeValueLists(
        AttributeDefinition<StringData>{.name = "s", .values = {"a"_sd, "b"_sd}},
        AttributeDefinition<std::span<bool>>{.name = "flag", .values = {boolArr1, boolArr2}});
    EXPECT_THAT(safeMakeAttributeTuples(owned),
                UnorderedElementsAre(
                    IsAttributesTuple(std::make_tuple("a"_sd, std::span<bool>{boolArr1})),
                    IsAttributesTuple(std::make_tuple("a"_sd, std::span<bool>{boolArr2})),
                    IsAttributesTuple(std::make_tuple("b"_sd, std::span<bool>{boolArr1})),
                    IsAttributesTuple(std::make_tuple("b"_sd, std::span<bool>{boolArr2}))));
}

TEST(SafeMakeAttributeTuplesOwnedTest, ThrowsOnDuplicateValues) {
    OwnedAttributeValueLists<int32_t> owned =
        makeOwnedAttributeValueLists(AttributeDefinition<int32_t>{.name = "n", .values = {1, 1}});
    ASSERT_THROWS_CODE(safeMakeAttributeTuples(owned), DBException, ErrorCodes::BadValue);
}

TEST(MakeComparableAttributeDefinitionTest, BoolAttribute) {
    auto result = makeComparableAttributeDefinition(
        AttributeDefinition<bool>{.name = "flag", .values = {true, false}});
    EXPECT_EQ(result.name, "flag");
    EXPECT_EQ(result.typeIndex, std::type_index(typeid(bool)));
    EXPECT_THAT(result.formattedValues, ElementsAre("true", "false"));
}

TEST(MakeComparableAttributeDefinitionTest, Int64Attribute) {
    auto result = makeComparableAttributeDefinition(
        AttributeDefinition<int64_t>{.name = "count", .values = {1, 2, 3}});
    EXPECT_EQ(result.name, "count");
    EXPECT_EQ(result.typeIndex, std::type_index(typeid(int64_t)));
    EXPECT_THAT(result.formattedValues, ElementsAre("1", "2", "3"));
}

TEST(MakeComparableAttributeDefinitionTest, StringDataAttribute) {
    auto result = makeComparableAttributeDefinition(
        AttributeDefinition<StringData>{.name = "env", .values = {"prod"_sd, "staging"_sd}});
    EXPECT_EQ(result.name, "env");
    EXPECT_EQ(result.typeIndex, std::type_index(typeid(StringData)));
    EXPECT_THAT(result.formattedValues, ElementsAre("prod", "staging"));
}

TEST(MakeComparableAttributeDefinitionTest, EqualWhenSameTypeAndValues) {
    auto a = makeComparableAttributeDefinition(
        AttributeDefinition<bool>{.name = "flag", .values = {true, false}});
    auto b = makeComparableAttributeDefinition(
        AttributeDefinition<bool>{.name = "flag", .values = {true, false}});
    EXPECT_EQ(a, b);
}

TEST(MakeComparableAttributeDefinitionTest, NotEqualWhenSameNameDifferentType) {
    // "true"/"false" format identically for bool and StringData, so only typeIndex distinguishes.
    auto boolDef = makeComparableAttributeDefinition(
        AttributeDefinition<bool>{.name = "flag", .values = {true, false}});
    auto stringDef = makeComparableAttributeDefinition(
        AttributeDefinition<StringData>{.name = "flag", .values = {"true"_sd, "false"_sd}});
    EXPECT_NE(boolDef, stringDef);
}

TEST(MakeComparableAttributeDefinitionTest, NotEqualWhenSameNameSameTypeButDifferentValues) {
    auto a = makeComparableAttributeDefinition(
        AttributeDefinition<int64_t>{.name = "count", .values = {1, 2}});
    auto b = makeComparableAttributeDefinition(
        AttributeDefinition<int64_t>{.name = "count", .values = {1, 2, 3}});
    EXPECT_NE(a, b);
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


TEST(AttributesKeyValueIterableTest, EmptyAttributes) {
    AttributesKeyValueIterable empty(std::vector<AttributeNameAndValue>{});
    EXPECT_EQ(empty.size(), 0u);
    EXPECT_THAT(empty, IsEmpty());
}

TEST(AttributesKeyValueIterableTest, SingleAttribute) {
    AttributesKeyValueIterable attrs(
        std::vector<AttributeNameAndValue>{{.name = "count", .value = int64_t{42}}});
    EXPECT_EQ(attrs.size(), 1u);
    EXPECT_THAT(attrs, ElementsAre(AttributeNameAndValue{.name = "count", .value = int64_t{42}}));
}

TEST(AttributesKeyValueIterableTest, MultipleAttributes) {
    AttributesKeyValueIterable attrs(std::vector<AttributeNameAndValue>{
        {.name = "count", .value = int64_t{42}},
        {.name = "flag", .value = true},
    });
    EXPECT_EQ(attrs.size(), 2u);
    EXPECT_THAT(attrs,
                ElementsAre(AttributeNameAndValue{.name = "count", .value = int64_t{42}},
                            AttributeNameAndValue{.name = "flag", .value = true}));
}

#ifdef MONGO_CONFIG_OTEL
using OtelAttributeValue = opentelemetry::common::AttributeValue;

struct KeyAndOtelValue {
    std::string key;
    OtelAttributeValue value;
};

// Matches against a KeyAndOtelValue struct with the given key and value matchers.
MATCHER_P2(KeyAndOtelValueIs, keyMatcher, valueMatcher, "") {
    return testing::ExplainMatchResult(keyMatcher, arg.key, result_listener) &&
        testing::ExplainMatchResult(valueMatcher, arg.value, result_listener);
}

TEST(AttributesKeyValueIterableTest, ForEachKeyValueEmpty) {
    AttributesKeyValueIterable empty(std::vector<AttributeNameAndValue>{});
    bool completed =
        empty.ForEachKeyValue([](std::string_view, OtelAttributeValue) noexcept { return true; });
    EXPECT_TRUE(completed);
}

TEST(AttributesKeyValueIterableTest, ForEachKeyValueBool) {
    AttributesKeyValueIterable attrs(
        std::vector<AttributeNameAndValue>{{.name = "flag", .value = true}});
    std::vector<KeyAndOtelValue> result;
    attrs.ForEachKeyValue([&](std::string_view key, OtelAttributeValue value) noexcept {
        result.push_back({std::string(key), value});
        return true;
    });
    EXPECT_THAT(result, ElementsAre(KeyAndOtelValueIs("flag", VariantWith<bool>(true))));
}

TEST(AttributesKeyValueIterableTest, ForEachKeyValueInt32) {
    AttributesKeyValueIterable attrs(
        std::vector<AttributeNameAndValue>{{.name = "x", .value = int32_t{7}}});
    std::vector<KeyAndOtelValue> result;
    attrs.ForEachKeyValue([&](std::string_view key, OtelAttributeValue value) noexcept {
        result.push_back({std::string(key), value});
        return true;
    });
    EXPECT_THAT(result, ElementsAre(KeyAndOtelValueIs("x", VariantWith<int32_t>(7))));
}

TEST(AttributesKeyValueIterableTest, ForEachKeyValueInt64) {
    AttributesKeyValueIterable attrs(
        std::vector<AttributeNameAndValue>{{.name = "count", .value = int64_t{999}}});
    std::vector<KeyAndOtelValue> result;
    attrs.ForEachKeyValue([&](std::string_view key, OtelAttributeValue value) noexcept {
        result.push_back({std::string(key), value});
        return true;
    });
    EXPECT_THAT(result, ElementsAre(KeyAndOtelValueIs("count", VariantWith<int64_t>(999))));
}

TEST(AttributesKeyValueIterableTest, ForEachKeyValueDouble) {
    AttributesKeyValueIterable attrs(
        std::vector<AttributeNameAndValue>{{.name = "ratio", .value = 3.14}});
    std::vector<KeyAndOtelValue> result;
    attrs.ForEachKeyValue([&](std::string_view key, OtelAttributeValue value) noexcept {
        result.push_back({std::string(key), value});
        return true;
    });
    EXPECT_THAT(result, ElementsAre(KeyAndOtelValueIs("ratio", VariantWith<double>(3.14))));
}

TEST(AttributesKeyValueIterableTest, ForEachKeyValueStringData) {
    std::string str = "hello";
    AttributesKeyValueIterable attrs(
        std::vector<AttributeNameAndValue>{{.name = "greeting", .value = StringData(str)}});
    std::vector<KeyAndOtelValue> result;
    attrs.ForEachKeyValue([&](std::string_view key, OtelAttributeValue value) noexcept {
        result.push_back({std::string(key), value});
        return true;
    });
    EXPECT_THAT(result,
                ElementsAre(KeyAndOtelValueIs("greeting", VariantWith<std::string_view>("hello"))));
}

TEST(AttributesKeyValueIterableTest, ForEachKeyValueStringDataSpan) {
    std::vector<StringData> strings = {"hello", "world"};
    AttributesKeyValueIterable attrs(
        std::vector<AttributeNameAndValue>{{.name = "greeting", .value = strings}});
    std::vector<std::string> result;
    attrs.ForEachKeyValue([&](std::string_view key, OtelAttributeValue value) noexcept {
        ASSERT_THAT(value, VariantWith<std::span<const std::string_view>>(_));
        for (const std::string_view s : std::get<std::span<const std::string_view>>(value)) {
            result.push_back(std::string(s));
        }
        return true;
    });
    EXPECT_THAT(result, ElementsAre("hello", "world"));
}

TEST(AttributesKeyValueIterableTest, ForEachKeyValueMultipleAttributes) {
    AttributesKeyValueIterable attrs(std::vector<AttributeNameAndValue>{
        {.name = "count", .value = int64_t{1}},
        {.name = "flag", .value = true},
    });
    std::vector<KeyAndOtelValue> result;
    attrs.ForEachKeyValue([&](std::string_view key, OtelAttributeValue value) noexcept {
        result.push_back({std::string(key), value});
        return true;
    });
    EXPECT_THAT(result,
                UnorderedElementsAre(KeyAndOtelValueIs("count", VariantWith<int64_t>(1)),
                                     KeyAndOtelValueIs("flag", VariantWith<bool>(true))));
}

TEST(AttributesKeyValueIterableTest, ForEachKeyValueCallbackAbort) {
    AttributesKeyValueIterable attrs(std::vector<AttributeNameAndValue>{
        {.name = "a", .value = int64_t{1}},
        {.name = "b", .value = int64_t{2}},
    });
    int callCount = 0;
    bool completed = attrs.ForEachKeyValue([&](std::string_view, OtelAttributeValue) noexcept {
        ++callCount;
        return false;
    });
    EXPECT_FALSE(completed);
    EXPECT_EQ(callCount, 1);
}
#endif  // MONGO_CONFIG_OTEL

}  // namespace
}  // namespace mongo::otel::metrics
