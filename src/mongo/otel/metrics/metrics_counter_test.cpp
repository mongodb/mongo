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

#include "mongo/otel/metrics/metrics_counter.h"

#include "mongo/config.h"
#include "mongo/otel/metrics/metrics_attributes_test_utils.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <vector>

namespace mongo::otel::metrics {
namespace {
using testing::_;
using testing::DoubleEq;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::UnorderedElementsAre;

template <typename T>
class CounterImplTest : public testing::Test {};

using CounterTypes = testing::Types<int64_t, double>;
TYPED_TEST_SUITE(CounterImplTest, CounterTypes);

TYPED_TEST(CounterImplTest, Adds) {
    CounterImpl<TypeParam> counter;
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    counter.add(1);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 1)));
    counter.add(10);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 11)));
}

TYPED_TEST(CounterImplTest, AddsZero) {
    CounterImpl<TypeParam> counter;
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    counter.add(0);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    counter.add(10);
    counter.add(0);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 10)));
}

TYPED_TEST(CounterImplTest, ExceptionOnNegativeAdd) {
    CounterImpl<TypeParam> counter;
    counter.add(1);
    ASSERT_THROWS_CODE(counter.add(-1), DBException, ErrorCodes::BadValue);
}

// Any issues with thread safety should be caught by tsan on this test.
TYPED_TEST(CounterImplTest, ConcurrentAdds) {
    CounterImpl<TypeParam> counter;
    constexpr int kNumThreads = 10;
    constexpr int kIncrementsPerThread = 1000;

    std::vector<stdx::thread> threads;
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([&counter]() {
            for (int j = 0; j < kIncrementsPerThread; ++j) {
                counter.add(1);
            }
        });
    }

    for (stdx::thread& thread : threads) {
        thread.join();
    }

    EXPECT_THAT(counter.values(),
                ElementsAre(IsAttributesAndValue(IsEmpty(), kNumThreads * kIncrementsPerThread)));
}

TYPED_TEST(CounterImplTest, Serialization) {
    CounterImpl<TypeParam> counter;
    const std::string key = "a";
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 0));
    counter.add(0);
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 0));
    counter.add(10);
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 10));
}

TYPED_TEST(CounterImplTest, SerializationWithAttributesAggregates) {
    CounterImpl<TypeParam, int32_t> counter({.name = "is_cool", .values = {1, 2, 3}});
    const std::string key = "a";
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 0));
    counter.add(0, {1});
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 0));
    counter.add(1, {2});
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 1));
    counter.add(10, {3});
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 11));
}

TYPED_TEST(CounterImplTest, ValuesContainsZeroNoAttributes) {
    CounterImpl<TypeParam> counter;
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
    counter.add(0);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 0)));
}

TYPED_TEST(CounterImplTest, ValuesSkipsZeroForAttributedCounter) {
    CounterImpl<TypeParam, bool> counter({.name = "is_cool", .values = {true, false}});
    EXPECT_THAT(counter.values(), IsEmpty());
    counter.add(0, {true});
    EXPECT_THAT(counter.values(), IsEmpty());
}

TYPED_TEST(CounterImplTest, ValuesNoAttributes) {
    CounterImpl<TypeParam> counter;
    counter.add(5);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), 5)));
}

TYPED_TEST(CounterImplTest, ThrowsIfAttributeNamesDuplicated) {
    ASSERT_THROWS_CODE(
        (CounterImpl<TypeParam, bool, bool, bool, bool>({.name = "is_cool1", .values = {true}},
                                                        {.name = "is_cool2", .values = {true}},
                                                        {.name = "is_cool3", .values = {true}},
                                                        {.name = "is_cool2", .values = {true}})),
        DBException,
        ErrorCodes::BadValue);
}

TYPED_TEST(CounterImplTest, ThrowsOnInvalidAttributes) {
    ASSERT_THROWS_CODE((CounterImpl<TypeParam, bool>({.name = "is_cool", .values = {true, true}})),
                       DBException,
                       ErrorCodes::BadValue);
}

TYPED_TEST(CounterImplTest, AddWithSingleAttribute) {
    CounterImpl<TypeParam, bool> counter({.name = "is_cool", .values = {true, false}});

    counter.add(5, {true});
    counter.add(3, {false});
    counter.add(2, {true});

    EXPECT_THAT(counter.values(),
                UnorderedElementsAre(
                    IsAttributesAndValue(
                        ElementsAre(AttributeNameAndValue{.name = "is_cool", .value = true}), 7),
                    IsAttributesAndValue(
                        ElementsAre(AttributeNameAndValue{.name = "is_cool", .value = false}), 3)));
}

TYPED_TEST(CounterImplTest, AddWithMultipleAttributes) {
    CounterImpl<TypeParam, bool, int64_t> counter({.name = "is_cool", .values = {true, false}},
                                                  {.name = "size", .values = {1, 2}});

    counter.add(5, {true, 1});
    counter.add(3, {false, 2});

    EXPECT_THAT(
        counter.values(),
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

TYPED_TEST(CounterImplTest, ExceptionOnUndeclaredAttributes) {
    CounterImpl<TypeParam, int64_t> counter({.name = "size", .values = {1, 2}});

    ASSERT_THROWS_CODE(counter.add(1, {3}), DBException, ErrorCodes::BadValue);
}

TYPED_TEST(CounterImplTest, ValuesSkipsZeroAttributes) {
    CounterImpl<TypeParam, bool> counter({.name = "is_cool", .values = {true, false}});

    counter.add(5, {true});

    EXPECT_THAT(counter.values(),
                ElementsAre(IsAttributesAndValue(
                    ElementsAre(AttributeNameAndValue{.name = "is_cool", .value = true}), 5)));
}

TYPED_TEST(CounterImplTest, StringDataAttributeValueIsCopied) {
    // If the source strings are destroyed after counter creation, the counter must have its own
    // copies and remain valid. Sanitizer builds will catch use-after-free if it does not.
    auto sourceValues = std::make_unique<std::vector<std::string>>(
        std::initializer_list<std::string>{"foo", "bar"});
    CounterImpl<TypeParam, StringData> counter(
        {.name = "temperature", .values = {(*sourceValues)[0], (*sourceValues)[1]}});
    sourceValues = nullptr;

    counter.add(5, {"foo"_sd});
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(_, 5)));
}

TYPED_TEST(CounterImplTest, SpanAttributeValueIsCopied) {
    // If the source span data is destroyed after counter creation, the counter must have its own
    // copies and remain valid. Sanitizer builds will catch use-after-free if it does not.
    auto sourceIntData = std::make_unique<std::vector<std::vector<int32_t>>>(
        std::vector<std::vector<int32_t>>{{1, 2}, {3, 4}});
    auto string1 = std::make_unique<std::string>("a");
    auto string2 = std::make_unique<std::string>("b");
    auto sourceStringData = std::make_unique<std::vector<std::vector<StringData>>>(
        std::vector<std::vector<StringData>>{{*string1}, {*string1, *string2}});
    CounterImpl<TypeParam, std::span<int32_t>, std::span<StringData>> counter(
        {.name = "intData",
         .values = {std::span<int32_t>((*sourceIntData)[0]),
                    std::span<int32_t>((*sourceIntData)[1])}},
        {.name = "stringData",
         .values = {std::span<StringData>((*sourceStringData)[0]),
                    std::span<StringData>((*sourceStringData)[1])}});
    sourceIntData = nullptr;
    string1 = nullptr;
    string2 = nullptr;
    sourceStringData = nullptr;

    std::vector<int32_t> intInput{1, 2};
    std::vector<StringData> stringInput{"a"_sd, "b"_sd};
    counter.add(5, {std::span<int32_t>(intInput), std::span<StringData>(stringInput)});
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(_, 5)));
}

TEST(DoubleCounterImplTest, AddsFractionalValues) {
    CounterImpl<double, bool> counter({.name = "is_cool", .values = {true, false}});

    counter.add(1.1, {true});
    counter.add(10.5, {false});
    counter.add(2.2, {true});

    EXPECT_THAT(counter.values(),
                UnorderedElementsAre(IsAttributesAndValue(ElementsAre(AttributeNameAndValue{
                                                              .name = "is_cool", .value = true}),
                                                          DoubleEq(3.3)),
                                     IsAttributesAndValue(ElementsAre(AttributeNameAndValue{
                                                              .name = "is_cool", .value = false}),
                                                          DoubleEq(10.5))));
}

}  // namespace
}  // namespace mongo::otel::metrics
