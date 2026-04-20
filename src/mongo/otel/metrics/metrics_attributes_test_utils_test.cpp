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
#include "mongo/otel/metrics/metrics_attributes_test_utils.h"

#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo::otel::metrics {
namespace {

using testing::_;
using testing::AllOf;
using testing::ElementsAre;
using testing::Gt;
using testing::Not;

// Googletest uses operator== to check for equality
TEST(AttributeNameAndValueEqualsTest, Equals) {
    EXPECT_EQ((AttributeNameAndValue{.name = "foo", .value = 3}),
              (AttributeNameAndValue{.name = "foo", .value = 3}));

    EXPECT_EQ((AttributeNameAndValue{.name = "foo", .value = "bar"}),
              (AttributeNameAndValue{.name = "foo", .value = "bar"}));

    std::vector<int32_t> intVec1{1, 2, 3};
    std::vector<int32_t> intVec2 = intVec1;
    EXPECT_EQ((AttributeNameAndValue{.name = "foo", .value = intVec1}),
              (AttributeNameAndValue{.name = "foo", .value = intVec2}));

    std::vector<StringData> stringDataVec1{"foo", "bar", "baz"};
    // Make these explicit strings to help guarantee that they don't reside in the same spot in
    // memory.
    std::vector<std::string> stringVec{"foo", "bar", "baz"};
    std::vector<StringData> stringDataVec2{stringVec[0], stringVec[1], stringVec[2]};
    EXPECT_EQ((AttributeNameAndValue{.name = "foo", .value = stringDataVec1}),
              (AttributeNameAndValue{.name = "foo", .value = stringDataVec2}));
}

TEST(AttributeNameAndValueEqualsTest, NotEquals) {
    EXPECT_NE((AttributeNameAndValue{.name = "foo", .value = 3}),
              (AttributeNameAndValue{.name = "foo", .value = 4}));

    EXPECT_NE((AttributeNameAndValue{.name = "foo", .value = 3}),
              (AttributeNameAndValue{.name = "bar", .value = 3}));

    EXPECT_NE((AttributeNameAndValue{.name = "foo", .value = "bar"}),
              (AttributeNameAndValue{.name = "foo", .value = "baz"}));

    std::vector<int32_t> intVec1{1, 2, 3};
    std::vector<int32_t> intVec2{1, 3, 2};
    EXPECT_NE((AttributeNameAndValue{.name = "foo", .value = intVec1}),
              (AttributeNameAndValue{.name = "foo", .value = intVec2}));

    intVec2 = std::vector{1, 2};
    EXPECT_NE((AttributeNameAndValue{.name = "foo", .value = intVec1}),
              (AttributeNameAndValue{.name = "foo", .value = intVec2}));

    std::vector<StringData> stringDataVec1{"foo", "bar", "baz"};
    // Make these explicit strings to help guarantee that they don't reside in the same spot in
    // memory.
    std::vector<std::string> stringVec{"foo", "baz", "baz"};
    std::vector<StringData> stringDataVec2{stringVec[0], stringVec[1], stringVec[2]};
    EXPECT_NE((AttributeNameAndValue{.name = "foo", .value = stringDataVec1}),
              (AttributeNameAndValue{.name = "foo", .value = stringDataVec2}));
}

TEST(IsAttributesAndValueTest, MatchesAttributes) {
    EXPECT_THAT((AttributesAndValue<int32_t>{
                    .attributes = AttributesKeyValueIterable(std::vector<AttributeNameAndValue>{
                        AttributeNameAndValue{.name = "foo", .value = 1}}),
                    .value = 5}),
                AllOf(IsAttributesAndValue(
                          ElementsAre(AttributeNameAndValue{.name = "foo", .value = 1}), _),
                      Not(IsAttributesAndValue(
                          ElementsAre(AttributeNameAndValue{.name = "bar", .value = 1}), _)),
                      Not(IsAttributesAndValue(
                          ElementsAre(AttributeNameAndValue{.name = "foo", .value = 2}), _))));
}

TEST(IsAttributesAndValueTest, MatchesValue) {
    EXPECT_THAT((AttributesAndValue<int32_t>{
                    .attributes = AttributesKeyValueIterable(std::vector<AttributeNameAndValue>{
                        AttributeNameAndValue{.name = "foo", .value = 1}}),
                    .value = 5}),
                AllOf(IsAttributesAndValue(_, 5),
                      IsAttributesAndValue(_, Gt(4)),
                      Not(IsAttributesAndValue(_, 4))));
}

}  // namespace
}  // namespace mongo::otel::metrics
