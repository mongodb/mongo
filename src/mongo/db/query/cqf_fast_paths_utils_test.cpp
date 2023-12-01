/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <memory>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/cqf_fast_paths_utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::optimizer::fast_path {
namespace {

bool filterMatchesPattern(BSONObj& filter, BSONObj& pattern) {
    return FilterComparator::kInstance.compare(filter, pattern) == 0;
}

TEST(CqfFastPathsUtilsTest, FilterComparatorMatchesEqOnTopLevelField) {
    auto filter = fromjson("{a: \"123\"}");
    auto pattern = BSON("ignored" << 0);
    ASSERT_TRUE(filterMatchesPattern(filter, pattern));

    filter = fromjson("{a: {\"$eq\": \"123\"}}");
    pattern = BSON("ignored" << BSON("$eq" << 0));
    ASSERT_TRUE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorMatchesEqObj) {
    auto filter = fromjson("{a: {\"$eq\": {b: \"123\", c: 123}}}");
    auto pattern = BSON("ignored" << BSON("$eq" << 0));
    ASSERT_TRUE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorDoesNotMatchEqFiltersOfDifferentShapes) {
    auto filter = fromjson("{a: \"123\"}");
    auto pattern = BSON("ignored" << BSON("$eq" << 0));
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));

    filter = fromjson("{a: {\"$eq\": \"123\"}}");
    pattern = BSON("ignored" << 0);
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorMatchesLtOnTopLevelField) {
    auto filter = fromjson("{a: {\"$lt\": \"123\"}}");
    auto pattern = BSON("ignored" << BSON("$lt" << 0));
    ASSERT_TRUE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorDoesNotMatchLtOnNestedField) {
    auto filter = fromjson("{\"a.b.c\": {\"$lt\": \"123\"}}");
    auto pattern = BSON("ignored" << BSON("$lt" << 0));
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));

    filter = fromjson("{a: {b: {c: {\"$lt\": \"123\"}}}}");
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorDoesNotMatchDifferentOps) {
    auto filter = fromjson("{a: {\"$eq\": \"123\"}}");
    auto pattern = BSON("ignored" << BSON("$lt" << 0));
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));

    filter = fromjson("{a: {\"$gt\": \"123\"}}");
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorDoesNotMatchSameOpsWithDifferentSubExprs) {
    auto filter = fromjson(R"({a: {$eq: {$concat: ["1", "2"]}}})");
    auto pattern = BSON("ignored" << BSON("$eq" << 0));
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));

    filter = fromjson(R"({a: {$eq: "$field"}})");
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorDoesNotMatchSingleOpWithConjunction) {
    auto filter = fromjson(R"({ignored: {$lt: 5}, b: {$gt: 5}})");
    auto pattern = BSON("ignored" << BSON("$lt" << 0));
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorMatchesEmptyWithEmpty) {
    BSONObj filter{};
    BSONObj pattern{};
    ASSERT_TRUE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorDoesNotMatchEmptyWithNonEmpty) {
    BSONObj filter{};
    auto pattern = BSON("ignored" << 0);
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorDoesNotMatchEquivalentConjunctionsInDifferentOrder) {
    // This test illustrates a limitation of the comparator where the ordering of predicates
    // matters.
    auto filter = fromjson(R"({f1: {$gt: 5}, f2: {$lt: 10}})");
    auto pattern = fromjson(R"({ignore: {$lt: 10}, ignore: {$gt: 5}})");
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));

    filter = fromjson(R"({ignore: {$lt: 1, $gt: 1}})");
    pattern = fromjson(R"({f1: {$gt: 1, $lt: 1}})");
    ASSERT_FALSE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorMatchesConstantContainingSpecialFields) {
    auto filter = fromjson(R"({ignored: {$eq: {_id: 1, "a.b.c": 2}}})");
    auto pattern = BSON("ignored" << BSON("$eq" << 0));
    ASSERT_TRUE(filterMatchesPattern(filter, pattern));
}

TEST(CqfFastPathsUtilsTest, FilterComparatorMatchesConjunctionWithDifferentConstants) {
    auto filter = fromjson(R"({a: {$lt: {x: 10}, $gt: {y: 50}}})");
    auto pattern = fromjson(R"({ignored: {$lt: 0, $gt: 0}})");
    ASSERT_TRUE(filterMatchesPattern(filter, pattern));
}

}  // namespace
}  // namespace mongo::optimizer::fast_path
