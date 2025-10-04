/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/shard_key_pattern_query_util.h"
#include "mongo/db/hasher.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const double INF = std::numeric_limits<double>::infinity();

/**
 * Test the chunk manager index bounds for query functionality.
 */
class CMCollapseTreeTest : public ShardingTestFixture {
protected:
    // Utility function to create a CanonicalQuery
    std::unique_ptr<CanonicalQuery> canonicalize(const char* queryStr) {
        BSONObj queryObj = fromjson(queryStr);
        const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.foo");
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(queryObj);
        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx =
                ExpressionContextBuilder{}.fromRequest(operationContext(), *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{
                .findCommand = std::move(findCommand),
                .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});
    }

    void checkIndexBoundsWithKey(const char* keyStr,
                                 std::unique_ptr<CanonicalQuery> query,
                                 const IndexBounds& expectedBounds) {
        ASSERT(query.get() != nullptr);

        BSONObj key = fromjson(keyStr);

        IndexBounds indexBounds = getIndexBoundsForQuery(key, *query.get());
        ASSERT_EQUALS(indexBounds.size(), expectedBounds.size());
        for (size_t i = 0; i < indexBounds.size(); i++) {
            const OrderedIntervalList& oil = indexBounds.fields[i];
            const OrderedIntervalList& expectedOil = expectedBounds.fields[i];
            ASSERT_EQUALS(oil.intervals.size(), expectedOil.intervals.size());
            for (size_t i = 0; i < oil.intervals.size(); i++) {
                if (Interval::INTERVAL_EQUALS !=
                    oil.intervals[i].compare(expectedOil.intervals[i])) {
                    LOGV2(22676,
                          "Found mismatching field interval",
                          "queryFieldInterval"_attr = oil.intervals[i].toString(false),
                          "expectedFieldInterval"_attr = expectedOil.intervals[i].toString(false));
                }
                ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                              oil.intervals[i].compare(expectedOil.intervals[i]));
            }
        }
    }

    void checkIndexBoundsWithKey(const char* keyStr,
                                 const char* queryStr,
                                 const IndexBounds& expectedBounds) {
        checkIndexBoundsWithKey(keyStr, canonicalize(queryStr), expectedBounds);
    }

    // Assume shard key is { a: 1 }
    void checkIndexBounds(const char* queryStr, const OrderedIntervalList& expectedOil) {
        auto query(canonicalize(queryStr));
        ASSERT(query.get() != nullptr);

        BSONObj key = fromjson("{a: 1}");

        IndexBounds indexBounds = getIndexBoundsForQuery(key, *query.get());
        ASSERT_EQUALS(indexBounds.size(), 1U);
        const OrderedIntervalList& oil = indexBounds.fields.front();

        if (oil.intervals.size() != expectedOil.intervals.size()) {
            LOGV2(22677,
                  "Found mismatching field intervals",
                  "queryFieldInterval"_attr = oil.toString(false),
                  "expectedFieldInterval"_attr = expectedOil.toString(false));
        }

        ASSERT_EQUALS(oil.intervals.size(), expectedOil.intervals.size());
        for (size_t i = 0; i < oil.intervals.size(); i++) {
            ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                          oil.intervals[i].compare(expectedOil.intervals[i]));
        }
    }
};

// { a: 2 } -> a: [2, 2]
TEST_F(CMCollapseTreeTest, Basic) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 2 << "" << 2), true, true));
    checkIndexBounds("{a: 2}", expected);
}

// { b: 2 } -> a: [MinKey, MaxKey]
TEST_F(CMCollapseTreeTest, AllValue) {
    OrderedIntervalList expected;
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expected.intervals.push_back(Interval(builder.obj(), true, true));
    checkIndexBounds("{b: 2}", expected);
}

// { 'a' : { '$not' : { '$gt' : 1 } } } -> a: [MinKey, 1.0], (inf, MaxKey]
TEST_F(CMCollapseTreeTest, NegativeGT) {
    OrderedIntervalList expected;
    {
        BSONObjBuilder builder;
        builder.appendMinKey("");
        builder.appendNumber("", 1.0);
        expected.intervals.push_back(Interval(builder.obj(), true, true));
    }
    {
        BSONObjBuilder builder;
        builder.append("", std::numeric_limits<double>::infinity());
        builder.appendMaxKey("");
        expected.intervals.push_back(Interval(builder.obj(), false, true));
    }
    checkIndexBounds("{ 'a' : { '$not' : { '$gt' : 1 } } }", expected);
}

// {$or: [{a: 20}, {$and: [{a:1}, {b:7}]}]} -> a: [1.0, 1.0], [20.0, 20.0]
TEST_F(CMCollapseTreeTest, OrWithAndChild) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 1.0 << "" << 1.0), true, true));
    expected.intervals.push_back(Interval(BSON("" << 20.0 << "" << 20.0), true, true));
    checkIndexBounds("{$or: [{a: 20}, {$and: [{a:1}, {b:7}]}]}", expected);
}

// {a:20, $or: [{b:1}, {c:7}]} -> a: [20.0, 20.0]
TEST_F(CMCollapseTreeTest, AndWithUnindexedOrChild) {
    // Logic rewrite could give a tree with root OR.
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 20.0 << "" << 20.0), true, true));
    checkIndexBounds("{a:20, $or: [{b:1}, {c:7}]}", expected);
}

// {$or: [{a:{$gt:2,$lt:10}}, {a:{$gt:0,$lt:5}}]} -> a: (0.0, 10.0)
TEST_F(CMCollapseTreeTest, OrOfAnd) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 0.0 << "" << 10.0), false, false));
    checkIndexBounds("{$or: [{a:{$gt:2,$lt:10}}, {a:{$gt:0,$lt:5}}]}", expected);
}

// {$or: [{a:{$gt:2,$lt:10}}, {a:{$gt:0,$lt:15}}, {a:{$gt:20}}]}
//   -> a: (0.0, 15.0), (20.0, inf]
TEST_F(CMCollapseTreeTest, OrOfAnd2) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 0.0 << "" << 15.0), false, false));
    expected.intervals.push_back(Interval(BSON("" << 20.0 << "" << INF), false, true));
    checkIndexBounds("{$or: [{a:{$gt:2,$lt:10}}, {a:{$gt:0,$lt:15}}, {a:{$gt:20}}]}", expected);
}

// "{$or: [{a:{$gt:1,$lt:5},b:6}, {a:3,b:{$gt:0,$lt:10}}]}" -> a: (1.0, 5.0)
TEST_F(CMCollapseTreeTest, OrOfAnd3) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 1.0 << "" << 5.0), false, false));
    checkIndexBounds("{$or: [{a:{$gt:1,$lt:5},b:6}, {a:3,b:{$gt:0,$lt:10}}]}", expected);
}

//
//  Compound shard key
//

// "{$or: [{a:{$gt:1,$lt:5}, b:{$gt:0,$lt:3}, c:6}, "
//        "{a:3, b:{$gt:1,$lt:2}, c:{$gt:0,$lt:10}}]}",
// -> a: (1.0, 5.0), b: (0.0, 3.0)
TEST_F(CMCollapseTreeTest, OrOfAnd4) {
    IndexBounds expectedBounds;
    expectedBounds.fields.push_back(OrderedIntervalList());
    expectedBounds.fields.push_back(OrderedIntervalList());

    expectedBounds.fields[0].intervals.push_back(
        Interval(BSON("" << 1.0 << "" << 5.0), false, false));
    expectedBounds.fields[1].intervals.push_back(
        Interval(BSON("" << 0.0 << "" << 3.0), false, false));

    checkIndexBoundsWithKey("{a: 1, b: 1}",  // shard key
                            "{$or: [{a:{$gt:1,$lt:5}, b:{$gt:0,$lt:3}, c:6}, "
                            "{a:3, b:{$gt:1,$lt:2}, c:{$gt:0,$lt:10}}]}",
                            expectedBounds);
}

// "{$or: [{a:{$gt:1,$lt:5}, c:6}, "
//        "{a:3, b:{$gt:1,$lt:2}, c:{$gt:0,$lt:10}}]}"));
// ->
TEST_F(CMCollapseTreeTest, OrOfAnd5) {
    IndexBounds expectedBounds;
    expectedBounds.fields.push_back(OrderedIntervalList());
    expectedBounds.fields.push_back(OrderedIntervalList());

    expectedBounds.fields[0].intervals.push_back(
        Interval(BSON("" << 1.0 << "" << 5.0), false, false));
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expectedBounds.fields[1].intervals.push_back(Interval(builder.obj(), true, true));

    checkIndexBoundsWithKey("{a: 1, b: 1}",  // shard key
                            "{$or: [{a:{$gt:1,$lt:5}, c:6}, "
                            "{a:3, b:{$gt:1,$lt:2}, c:{$gt:0,$lt:10}}]}",
                            expectedBounds);
}

// {$or: [{a:{$in:[1]},b:{$in:[1]}}, {a:{$in:[1,5]},b:{$in:[1,5]}}]}
// -> a: [1], [5]; b: [1], [5]
TEST_F(CMCollapseTreeTest, OrOfAnd6) {
    IndexBounds expectedBounds;
    expectedBounds.fields.push_back(OrderedIntervalList());
    expectedBounds.fields.push_back(OrderedIntervalList());

    // a: [1], [5]
    expectedBounds.fields[0].intervals.push_back(
        Interval(BSON("" << 1.0 << "" << 1.0), true, true));
    expectedBounds.fields[0].intervals.push_back(
        Interval(BSON("" << 5.0 << "" << 5.0), true, true));

    // b: [1], [5]
    expectedBounds.fields[1].intervals.push_back(
        Interval(BSON("" << 1.0 << "" << 1.0), true, true));
    expectedBounds.fields[1].intervals.push_back(
        Interval(BSON("" << 5.0 << "" << 5.0), true, true));

    checkIndexBoundsWithKey("{a: 1, b: 1}",  // shard key
                            "{$or: [{a:{$in:[1]},b:{$in:[1]}}, {a:{$in:[1,5]},b:{$in:[1,5]}}]}",
                            expectedBounds);
}

TEST_F(CMCollapseTreeTest, SortInReverseDirection) {
    IndexBounds expected;
    expected.fields.push_back(OrderedIntervalList());
    expected.fields.push_back(OrderedIntervalList());

    expected.fields[0].intervals.push_back(Interval(BSON("" << 10 << "" << 20), true, true));
    expected.fields[1].intervals.push_back(Interval(BSON("" << 100 << "" << 200), true, true));

    // constructing query
    BSONObj queryObj = fromjson("{a: {$gte: 10, $lte: 20}, b: {$gte: 100, $lte: 200}}");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.foo");
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(queryObj);
    findCommand->setSort(BSON("a" << -1 << "b" << -1));
    auto query = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(operationContext(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{
            .findCommand = std::move(findCommand),
            .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});

    checkIndexBoundsWithKey("{a: 1, b: 1}",  // shard key
                            std::move(query),
                            expected);
}

TEST_F(CMCollapseTreeTest, SortMergeFromOrInReverseDirection) {
    IndexBounds expected;
    expected.fields.push_back(OrderedIntervalList());
    expected.fields.push_back(OrderedIntervalList());
    expected.fields.push_back(OrderedIntervalList());

    expected.fields[0].intervals.push_back(Interval(BSON("" << "foo"
                                                            << ""
                                                            << "foo"),
                                                    true,
                                                    true));
    expected.fields[1].intervals.push_back(Interval(BSON("" << "bar"
                                                            << ""
                                                            << "bar"),
                                                    true,
                                                    true));
    expected.fields[1].intervals.push_back(Interval(BSON("" << "baz"
                                                            << ""
                                                            << "baz"),
                                                    true,
                                                    true));
    expected.fields[2].intervals.push_back(Interval(BSON("" << 100 << "" << 200), true, true));

    // constructing query
    BSONObj queryObj =
        fromjson("{a: \"foo\", b: {$in: [\"bar\", \"baz\"]}, c: {$gte: 100, $lte: 200}}");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.foo");
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(queryObj);
    findCommand->setLimit(1);
    findCommand->setSort(BSON("c" << -1));
    auto query = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(operationContext(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{
            .findCommand = std::move(findCommand),
            .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});

    checkIndexBoundsWithKey("{a: 1, b: 1, c: 1}",  // shard key
                            std::move(query),
                            expected);
}

//
// Array operators
//

// {a : {$elemMatch: {b:1}}} -> a.b: [MinKey, MaxKey]
// Shard key doesn't allow multikey, but query on array should succeed without error.
TEST_F(CMCollapseTreeTest, ElemMatchOneField) {
    IndexBounds expectedBounds;
    expectedBounds.fields.push_back(OrderedIntervalList());
    OrderedIntervalList& oil = expectedBounds.fields.front();
    oil.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));
    checkIndexBoundsWithKey("{'a.b': 1}", "{a : {$elemMatch: {b:1}}}", expectedBounds);
}

// {foo: {$all: [ {$elemMatch: {a:1, b:1}}, {$elemMatch: {a:2, b:2}}]}}
//    -> foo.a: [1, 1]
// Or -> foo.a: [2, 2]
TEST_F(CMCollapseTreeTest, BasicAllElemMatch) {
    Interval expectedInterval(BSON("" << 1 << "" << 1), true, true);

    const char* queryStr = "{foo: {$all: [ {$elemMatch: {a:1, b:1}} ]}}";
    auto query(canonicalize(queryStr));
    ASSERT(query.get() != nullptr);

    BSONObj key = fromjson("{'foo.a': 1}");

    IndexBounds indexBounds = getIndexBoundsForQuery(key, *query.get());
    ASSERT_EQUALS(indexBounds.size(), 1U);
    const OrderedIntervalList& oil = indexBounds.fields.front();
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    const Interval& interval = oil.intervals.front();

    // Choose one of the two possible solutions.
    // Two solutions differ only by assignment of index tags.
    ASSERT(Interval::INTERVAL_EQUALS == interval.compare(expectedInterval));
}

// {a : [1, 2, 3]} -> a: [1, 1], [[1, 2, 3], [1, 2, 3]]
TEST_F(CMCollapseTreeTest, ArrayEquality) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));
    BSONArray array(BSON_ARRAY(1 << 2 << 3));

    Interval interval(BSON("" << array << "" << array), true, true);
    expected.intervals.push_back(interval);
    checkIndexBounds("{a : [1, 2, 3]}", expected);
}


//
//  Features: Regex, $where, $text, hashed key
//

// { a: /abc/ } -> a: ["", {}), [/abc/, /abc/]
TEST_F(CMCollapseTreeTest, Regex) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << ""
                                                  << "" << BSONObj()),
                                          true,
                                          false));
    BSONObjBuilder builder;
    builder.appendRegex("", "abc");
    builder.appendRegex("", "abc");
    expected.intervals.push_back(Interval(builder.obj(), true, true));
    checkIndexBounds("{ a: /abc/ }", expected);
}

// {$where: 'this.credits == this.debits' }
TEST_F(CMCollapseTreeTest, Where) {
    OrderedIntervalList expected;
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expected.intervals.push_back(Interval(builder.obj(), true, true));
    checkIndexBounds("{$where: 'this.credits == this.debits' }", expected);
}

// { $text: { $search: "coffee -cake" } }
TEST_F(CMCollapseTreeTest, Text) {
    OrderedIntervalList expected;
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expected.intervals.push_back(Interval(builder.obj(), true, true));
    checkIndexBounds("{ $text: { $search: 'coffee -cake' } }", expected);
}

// { a: 2, $text: { $search: "leche", $language: "es" } }
TEST_F(CMCollapseTreeTest, TextWithQuery) {
    OrderedIntervalList expected;
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expected.intervals.push_back(Interval(builder.obj(), true, true));
    checkIndexBounds("{ a: 2, $text: { $search: 'leche', $language: 'es' } }", expected);
}

//  { a: 0 } -> hashed a: [hash(0), hash(0)]
TEST_F(CMCollapseTreeTest, HashedSinglePoint) {
    IndexBounds expectedBounds;
    expectedBounds.fields.push_back(OrderedIntervalList());
    const auto hashOfZero = BSONElementHasher::hash64(BSON("" << 0).firstElement(), 0);
    expectedBounds.fields[0].intervals.push_back(
        Interval(BSON("" << hashOfZero << "" << hashOfZero), true, true));
    checkIndexBoundsWithKey("{a: 'hashed'}", "{ a: 0}", expectedBounds);
}

// { a: { $lt: 2, $gt: 1} } -> hashed a: [Minkey, Maxkey]
TEST_F(CMCollapseTreeTest, HashedRange) {
    IndexBounds expectedBounds;
    expectedBounds.fields.push_back(OrderedIntervalList());
    OrderedIntervalList& expectedOil = expectedBounds.fields.front();
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expectedOil.intervals.push_back(Interval(builder.obj(), true, true));
    checkIndexBoundsWithKey("{a: 'hashed'}", "{ a: { $lt: 2, $gt: 1} }", expectedBounds);
}

// { a: /abc/ } -> hashed a: [Minkey, Maxkey]
TEST_F(CMCollapseTreeTest, HashedRegex) {
    IndexBounds expectedBounds;
    expectedBounds.fields.push_back(OrderedIntervalList());
    OrderedIntervalList& expectedOil = expectedBounds.fields.front();
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expectedOil.intervals.push_back(Interval(builder.obj(), true, true));

    checkIndexBoundsWithKey("{a: 'hashed'}", "{ a: /abc/ }", expectedBounds);
}

TEST_F(CMCollapseTreeTest, CompoundHashedShardKeyWithHashedPrefix) {
    const auto shardKey = "{a: 'hashed', b: 1}";
    const auto query = "{$or: [{a:{$in:[1]},b:{$in:[1]}}, {a:{$in:[1,5]},b:{$gt:0,$lt:3}}]}";

    IndexBounds expectedBounds;
    expectedBounds.fields.push_back(OrderedIntervalList());
    expectedBounds.fields.push_back(OrderedIntervalList());
    const auto hashOfOne = BSONElementHasher::hash64(BSON("" << 1.0).firstElement(), 0);
    const auto hashOfFive = BSONElementHasher::hash64(BSON("" << 5.0).firstElement(), 0);

    // a: [[hash(5), hash(5)], [hash(1), hash(1)]]. Note, hash(5) is less than hash(1), hence it
    // appears before hash(1).
    expectedBounds.fields[0].intervals.push_back(
        Interval(BSON("" << hashOfFive << "" << hashOfFive), true, true));
    expectedBounds.fields[0].intervals.push_back(
        Interval(BSON("" << hashOfOne << "" << hashOfOne), true, true));

    // b: (0,3)
    expectedBounds.fields[1].intervals.push_back(Interval(BSON("" << 0 << "" << 3), false, false));

    checkIndexBoundsWithKey(shardKey, query, expectedBounds);
}

TEST_F(CMCollapseTreeTest, CompoundHashedShardKeyWithRangePrefix) {
    const auto shardKey = "{a: 1, b: 'hashed', c: 1}";
    const auto query = "{a:{$gt:0,$lte:3}, b: 4}";

    IndexBounds expectedBounds;
    expectedBounds.fields.push_back(OrderedIntervalList());
    expectedBounds.fields.push_back(OrderedIntervalList());
    expectedBounds.fields.push_back(OrderedIntervalList());
    const auto hashOfFour = BSONElementHasher::hash64(BSON("" << 4.0).firstElement(), 0);

    // a: (0,3]
    expectedBounds.fields[0].intervals.push_back(Interval(BSON("" << 0 << "" << 3), false, true));
    // b: [hash(4), hash(4)]
    expectedBounds.fields[1].intervals.push_back(
        Interval(BSON("" << hashOfFour << "" << hashOfFour), true, true));
    // c: [MinKey, MaxKey]
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expectedBounds.fields[2].intervals.push_back(Interval(builder.obj(), true, true));

    checkIndexBoundsWithKey(shardKey, query, expectedBounds);
}

/**
 * Tests the KeyPattern key bounds generation logic.
 */
class CMKeyBoundsTest : public mongo::unittest::Test {
protected:
    void checkBoundList(const BoundList& list, const BoundList& expected) {
        ASSERT_EQUALS(list.size(), expected.size());
        for (size_t i = 0; i < list.size(); i++) {
            ASSERT_EQUALS(list[i].first.woCompare(expected[i].first), 0);
            ASSERT_EQUALS(list[i].second.woCompare(expected[i].second), 0);
        }
    }
};

// Key { a: 1 }, Bounds a: [0]
//  => { a: 0 } -> { a: 0 }
TEST_F(CMKeyBoundsTest, Basic) {
    IndexBounds indexBounds;
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.front().intervals.push_back(Interval(BSON("" << 0 << "" << 0), true, true));

    BoundList expectedList;
    expectedList.emplace_back(fromjson("{a: 0}"), fromjson("{a: 0}"));

    ShardKeyPattern skeyPattern(fromjson("{a: 1}"));
    BoundList list = flattenBounds(skeyPattern, indexBounds);
    checkBoundList(list, expectedList);
}

// Key { a: 1 }, Bounds a: [2, 3)
//  => { a: 2 } -> { a: 3 }  // bound inclusion is ignored.
TEST_F(CMKeyBoundsTest, SingleInterval) {
    IndexBounds indexBounds;
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.front().intervals.push_back(Interval(BSON("" << 2 << "" << 3), true, false));

    BoundList expectedList;
    expectedList.emplace_back(fromjson("{a: 2}"), fromjson("{a: 3}"));

    ShardKeyPattern skeyPattern(fromjson("{a: 1}"));
    BoundList list = flattenBounds(skeyPattern, indexBounds);
    checkBoundList(list, expectedList);
}

// Key { a: 1, b: 1, c: 1 }, Bounds a: [2, 3), b: [2, 3), c: [2: 3)
//  => { a: 2, b: 2, c: 2 } -> { a: 3, b: 3, c: 3 }
TEST_F(CMKeyBoundsTest, MultiIntervals) {
    IndexBounds indexBounds;
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields[0].intervals.push_back(Interval(BSON("" << 2 << "" << 3), true, false));
    indexBounds.fields[1].intervals.push_back(Interval(BSON("" << 2 << "" << 3), true, false));
    indexBounds.fields[2].intervals.push_back(Interval(BSON("" << 2 << "" << 3), true, false));

    BoundList expectedList;
    expectedList.emplace_back(fromjson("{ a: 2, b: 2, c: 2 }"), fromjson("{ a: 3, b: 3, c: 3 }"));

    ShardKeyPattern skeyPattern(fromjson("{a: 1, b: 1, c: 1}"));
    BoundList list = flattenBounds(skeyPattern, indexBounds);
    checkBoundList(list, expectedList);
}

// Key { a: 1, b: 1, c: 1 }, Bounds a: [0, 0], b: { $in: [4, 5, 6] }, c: [2: 3)
//  => { a: 0, b: 4, c: 2 } -> { a: 0, b: 4, c: 3 }
//     { a: 0, b: 5, c: 2 } -> { a: 0, b: 5, c: 3 }
//     { a: 0, b: 6, c: 2 } -> { a: 0, b: 6, c: 3 }
TEST_F(CMKeyBoundsTest, IntervalExpansion) {
    IndexBounds indexBounds;
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.push_back(OrderedIntervalList());

    indexBounds.fields[0].intervals.push_back(Interval(BSON("" << 0 << "" << 0), true, true));

    indexBounds.fields[1].intervals.push_back(Interval(BSON("" << 4 << "" << 4), true, true));
    indexBounds.fields[1].intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    indexBounds.fields[1].intervals.push_back(Interval(BSON("" << 6 << "" << 6), true, true));

    indexBounds.fields[2].intervals.push_back(Interval(BSON("" << 2 << "" << 3), true, false));

    BoundList expectedList;
    expectedList.emplace_back(fromjson("{ a: 0, b: 4, c: 2 }"), fromjson("{ a: 0, b: 4, c: 3 }"));
    expectedList.emplace_back(fromjson("{ a: 0, b: 5, c: 2 }"), fromjson("{ a: 0, b: 5, c: 3 }"));
    expectedList.emplace_back(fromjson("{ a: 0, b: 6, c: 2 }"), fromjson("{ a: 0, b: 6, c: 3 }"));

    ShardKeyPattern skeyPattern(fromjson("{a: 1, b: 1, c: 1}"));
    BoundList list = flattenBounds(skeyPattern, indexBounds);
    checkBoundList(list, expectedList);
}

// Key { a: 1, b: 1, c: 1 }, Bounds a: [0, 1], b: { $in: [4, 5, 6] }, c: [2: 3)
//  => { a: 0, b: 4, c: 2 } -> { a: 1, b: 6, c: 3 }
// Since field "a" is not a point, expasion after "a" is not allowed.
TEST_F(CMKeyBoundsTest, NonPointIntervalExpasion) {
    IndexBounds indexBounds;
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.push_back(OrderedIntervalList());

    indexBounds.fields[0].intervals.push_back(Interval(BSON("" << 0 << "" << 1), true, true));

    indexBounds.fields[1].intervals.push_back(Interval(BSON("" << 4 << "" << 4), true, true));
    indexBounds.fields[1].intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    indexBounds.fields[1].intervals.push_back(Interval(BSON("" << 6 << "" << 6), true, true));

    indexBounds.fields[2].intervals.push_back(Interval(BSON("" << 2 << "" << 3), true, false));

    BoundList expectedList;
    expectedList.emplace_back(fromjson("{ a: 0, b: 4, c: 2 }"), fromjson("{ a: 1, b: 6, c: 3 }"));

    ShardKeyPattern skeyPattern(fromjson("{a: 1, b: 1, c: 1}"));
    BoundList list = flattenBounds(skeyPattern, indexBounds);
    checkBoundList(list, expectedList);
}

/**
 * Tests the index bounds generation in the presence of a GEO_NEAR predicate.
 */
TEST_F(CMCollapseTreeTest, GeoNearDoesNotAffectOtherBounds) {
    // {a: 2, b: {$near: ...}} -> a: [2, 2]
    {
        OrderedIntervalList expected;
        expected.intervals.push_back(Interval(BSON("" << 2 << "" << 2), true, true));
        checkIndexBounds(
            "{a: 2, b: {$near: {$geometry: {type: \"Point\", coordinates: [0, 0]}, $minDistance: "
            "0, "
            "$maxDistance: 2}}}",
            expected);
    }
    // With compound index {a: 1, c: 1}
    // {a: 2, b: {$near: ...}, c: {$gt: 2}} -> a: [2, 2], c: (2, inf]
    {
        IndexBounds expectedBounds;
        expectedBounds.fields.push_back(OrderedIntervalList());
        expectedBounds.fields.push_back(OrderedIntervalList());

        expectedBounds.fields[0].intervals.push_back(
            Interval(BSON("" << 2 << "" << 2), true, true));

        BSONObjBuilder builder;
        builder.appendNumber("", 2);
        builder.append("", std::numeric_limits<double>::infinity());
        expectedBounds.fields[1].intervals.push_back(Interval(builder.obj(), false, true));

        checkIndexBoundsWithKey("{a: 1, c: 1}",  // shard key
                                "{a: 2, b: {$near: {$geometry: {type: \"Point\", coordinates: [0, "
                                "0]}, $minDistance: 0, "
                                "$maxDistance: 2}}, c: {$gt: 2}}",
                                expectedBounds);
    }
    // With compound index {a: 1, b: 1, c: 1}
    // {a: 2, b: {$near: ...}, c: {$gt: 2}} -> a: [2, 2], b: [MinKey, MaxKey], c: (2, inf]
    {
        IndexBounds expectedBounds;
        expectedBounds.fields.push_back(OrderedIntervalList());
        expectedBounds.fields.push_back(OrderedIntervalList());
        expectedBounds.fields.push_back(OrderedIntervalList());

        expectedBounds.fields[0].intervals.push_back(
            Interval(BSON("" << 2 << "" << 2), true, true));

        BSONObjBuilder bBuilder;
        bBuilder.appendMinKey("");
        bBuilder.appendMaxKey("");
        expectedBounds.fields[1].intervals.push_back(Interval(bBuilder.obj(), true, true));

        BSONObjBuilder cBuilder;
        cBuilder.appendNumber("", 2);
        cBuilder.append("", std::numeric_limits<double>::infinity());
        expectedBounds.fields[2].intervals.push_back(Interval(cBuilder.obj(), false, true));

        checkIndexBoundsWithKey("{a: 1, b: 1, c: 1}",  // shard key
                                "{a: 2, b: {$near: {$geometry: {type: \"Point\", coordinates: [0, "
                                "0]}, $minDistance: 0, "
                                "$maxDistance: 2}}, c: {$gt: 2}}",
                                expectedBounds);
    }
}

/**
 * The implementation of the above optimization assumes that there are some limitations on GEO_NEAR
 * predicates. If these limitations are removed in the future, we should refactor the optimization.
 */
TEST_F(CMCollapseTreeTest, GeoNearLimitationsInPlace) {
    // There can be at most one GEO_NEAR predicate.
    {
        BSONObj queryObj = fromjson(
            "{a: 2, b: {$near: {$geometry: {type: \"Point\", coordinates: [0, 0]}, $minDistance: "
            "0, $maxDistance: 2}}, c: {$near: {$geometry: {type: \"Point\", coordinates: [0, 0]}, "
            "$minDistance: 0, $maxDistance: 2}}}");
        const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.foo");
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(queryObj);
        ASSERT_THROWS_CODE(
            std::make_unique<CanonicalQuery>(CanonicalQueryParams{
                .expCtx = ExpressionContextBuilder{}
                              .fromRequest(operationContext(), *findCommand)
                              .build(),
                .parsedFind =
                    ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                            .allowedFeatures =
                                                MatchExpressionParser::kAllowAllSpecialFeatures}}),
            DBException,
            ErrorCodes::BadValue);
    }

    // GEO_NEAR must be a top-level expression in the CanonicalQuery.
    {
        BSONObj queryObj = fromjson(
            "{$or: {a: 2, b: {$near: {$geometry: {type: \"Point\", coordinates: [0, 0]}, "
            "$minDistance: 0, $maxDistance: 2}}}}");
        const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.foo");
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(queryObj);
        ASSERT_THROWS_CODE(
            std::make_unique<CanonicalQuery>(CanonicalQueryParams{
                .expCtx = ExpressionContextBuilder{}
                              .fromRequest(operationContext(), *findCommand)
                              .build(),
                .parsedFind =
                    ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                            .allowedFeatures =
                                                MatchExpressionParser::kAllowAllSpecialFeatures}}),
            DBException,
            ErrorCodes::BadValue);
    }
}

}  // namespace
}  // namespace mongo
