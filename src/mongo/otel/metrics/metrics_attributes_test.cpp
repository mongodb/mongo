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

#include "mongo/unittest/unittest.h"

#include <string>
#include <tuple>
#include <vector>

namespace mongo::otel::metrics {

using testing::ElementsAre;
using testing::IsEmpty;
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
        safeMakeAttributeTuples(std::vector<std::string>{"a", "b", "c"}, std::vector{1, 2});
    EXPECT_THAT(result,
                UnorderedElementsAre(std::make_tuple(std::string("a"), 1),
                                     std::make_tuple(std::string("a"), 2),
                                     std::make_tuple(std::string("b"), 1),
                                     std::make_tuple(std::string("b"), 2),
                                     std::make_tuple(std::string("c"), 1),
                                     std::make_tuple(std::string("c"), 2)));
}

TEST(SafeMakeAttributeTuplesTest, ThreeVectors) {
    auto result = safeMakeAttributeTuples(
        std::vector<std::string>{"a", "b"}, std::vector{1, 2}, std::vector{true, false});
    EXPECT_THAT(result,
                UnorderedElementsAre(std::make_tuple(std::string("a"), 1, true),
                                     std::make_tuple(std::string("a"), 1, false),
                                     std::make_tuple(std::string("a"), 2, true),
                                     std::make_tuple(std::string("a"), 2, false),
                                     std::make_tuple(std::string("b"), 1, true),
                                     std::make_tuple(std::string("b"), 1, false),
                                     std::make_tuple(std::string("b"), 2, true),
                                     std::make_tuple(std::string("b"), 2, false)));
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

}  // namespace mongo::otel::metrics
