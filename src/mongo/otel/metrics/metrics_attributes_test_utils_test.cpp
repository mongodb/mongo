// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/otel/metrics/metrics_attributes_test_utils.h"

#include "mongo/unittest/unittest.h"

#include <string>
#include <string_view>

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

    std::vector<std::string_view> stringDataVec1{"foo", "bar", "baz"};
    // Make these explicit strings to help guarantee that they don't reside in the same spot in
    // memory.
    std::vector<std::string> stringVec{"foo", "bar", "baz"};
    std::vector<std::string_view> stringDataVec2{stringVec[0], stringVec[1], stringVec[2]};
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

    std::vector<std::string_view> stringDataVec1{"foo", "bar", "baz"};
    // Make these explicit strings to help guarantee that they don't reside in the same spot in
    // memory.
    std::vector<std::string> stringVec{"foo", "baz", "baz"};
    std::vector<std::string_view> stringDataVec2{stringVec[0], stringVec[1], stringVec[2]};
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
