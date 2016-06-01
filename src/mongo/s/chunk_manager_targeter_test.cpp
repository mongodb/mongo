/**
 *    Copyright (C) 2014 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/json.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace {

using namespace mongo;

using std::unique_ptr;
using std::make_pair;

/**
 * ChunkManager targeting test
 *
 * TODO:
 *   Pull the implementation out of chunk.cpp
 */

// Utility function to create a CanonicalQuery
unique_ptr<CanonicalQuery> canonicalize(const char* queryStr) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    BSONObj queryObj = fromjson(queryStr);
    const NamespaceString nss("test.foo");
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(queryObj);
    auto statusWithCQ =
        CanonicalQuery::canonicalize(txn.get(), std::move(qr), ExtensionsCallbackNoop());
    ASSERT_OK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
}

void checkIndexBoundsWithKey(const char* keyStr,
                             const char* queryStr,
                             const IndexBounds& expectedBounds) {
    unique_ptr<CanonicalQuery> query(canonicalize(queryStr));
    ASSERT(query.get() != NULL);

    BSONObj key = fromjson(keyStr);

    IndexBounds indexBounds = ChunkManager::getIndexBoundsForQuery(key, *query.get());
    ASSERT_EQUALS(indexBounds.size(), expectedBounds.size());
    for (size_t i = 0; i < indexBounds.size(); i++) {
        const OrderedIntervalList& oil = indexBounds.fields[i];
        const OrderedIntervalList& expectedOil = expectedBounds.fields[i];
        ASSERT_EQUALS(oil.intervals.size(), expectedOil.intervals.size());
        for (size_t i = 0; i < oil.intervals.size(); i++) {
            if (Interval::INTERVAL_EQUALS != oil.intervals[i].compare(expectedOil.intervals[i])) {
                log() << oil.intervals[i] << " != " << expectedOil.intervals[i];
            }
            ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                          oil.intervals[i].compare(expectedOil.intervals[i]));
        }
    }
}

// Assume shard key is { a: 1 }
void checkIndexBounds(const char* queryStr, const OrderedIntervalList& expectedOil) {
    unique_ptr<CanonicalQuery> query(canonicalize(queryStr));
    ASSERT(query.get() != NULL);

    BSONObj key = fromjson("{a: 1}");

    IndexBounds indexBounds = ChunkManager::getIndexBoundsForQuery(key, *query.get());
    ASSERT_EQUALS(indexBounds.size(), 1U);
    const OrderedIntervalList& oil = indexBounds.fields.front();

    if (oil.intervals.size() != expectedOil.intervals.size()) {
        for (size_t i = 0; i < oil.intervals.size(); i++) {
            log() << oil.intervals[i];
        }
    }

    ASSERT_EQUALS(oil.intervals.size(), expectedOil.intervals.size());
    for (size_t i = 0; i < oil.intervals.size(); i++) {
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                      oil.intervals[i].compare(expectedOil.intervals[i]));
    }
}

const double INF = std::numeric_limits<double>::infinity();

// { a: 2 } -> a: [2, 2]
TEST(CMCollapseTreeTest, Basic) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 2 << "" << 2), true, true));
    checkIndexBounds("{a: 2}", expected);
}

// { b: 2 } -> a: [MinKey, MaxKey]
TEST(CMCollapseTreeTest, AllValue) {
    OrderedIntervalList expected;
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expected.intervals.push_back(Interval(builder.obj(), true, true));
    checkIndexBounds("{b: 2}", expected);
}

// { 'a' : { '$not' : { '$gt' : 1 } } } -> a: [MinKey, 1.0], (inf.0, MaxKey]
TEST(CMCollapseTreeTest, NegativeGT) {
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
TEST(CMCollapseTreeTest, OrWithAndChild) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 1.0 << "" << 1.0), true, true));
    expected.intervals.push_back(Interval(BSON("" << 20.0 << "" << 20.0), true, true));
    checkIndexBounds("{$or: [{a: 20}, {$and: [{a:1}, {b:7}]}]}", expected);
}

// {a:20, $or: [{b:1}, {c:7}]} -> a: [20.0, 20.0]
TEST(CMCollapseTreeTest, AndWithUnindexedOrChild) {
    // Logic rewrite could give a tree with root OR.
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 20.0 << "" << 20.0), true, true));
    checkIndexBounds("{a:20, $or: [{b:1}, {c:7}]}", expected);
}

// {$or: [{a:{$gt:2,$lt:10}}, {a:{$gt:0,$lt:5}}]} -> a: (0.0, 10.0)
TEST(CMCollapseTreeTest, OrOfAnd) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 0.0 << "" << 10.0), false, false));
    checkIndexBounds("{$or: [{a:{$gt:2,$lt:10}}, {a:{$gt:0,$lt:5}}]}", expected);
}

// {$or: [{a:{$gt:2,$lt:10}}, {a:{$gt:0,$lt:15}}, {a:{$gt:20}}]}
//   -> a: (0.0, 15.0), (20.0, inf.0]
TEST(CMCollapseTreeTest, OrOfAnd2) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON("" << 0.0 << "" << 15.0), false, false));
    expected.intervals.push_back(Interval(BSON("" << 20.0 << "" << INF), false, true));
    checkIndexBounds("{$or: [{a:{$gt:2,$lt:10}}, {a:{$gt:0,$lt:15}}, {a:{$gt:20}}]}", expected);
}

// "{$or: [{a:{$gt:1,$lt:5},b:6}, {a:3,b:{$gt:0,$lt:10}}]}" -> a: (1.0, 5.0)
TEST(CMCollapseTreeTest, OrOfAnd3) {
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
TEST(CMCollapseTreeTest, OrOfAnd4) {
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
TEST(CMCollapseTreeTest, OrOfAnd5) {
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
TEST(CMCollapseTreeTest, OrOfAnd6) {
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

//
// Array operators
//

// {a : {$elemMatch: {b:1}}} -> a.b: [MinKey, MaxKey]
// Shard key doesn't allow multikey, but query on array should succeed without error.
TEST(CMCollapseTreeTest, ElemMatchOneField) {
    IndexBounds expectedBounds;
    expectedBounds.fields.push_back(OrderedIntervalList());
    OrderedIntervalList& oil = expectedBounds.fields.front();
    oil.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));
    checkIndexBoundsWithKey("{'a.b': 1}", "{a : {$elemMatch: {b:1}}}", expectedBounds);
}

// {foo: {$all: [ {$elemMatch: {a:1, b:1}}, {$elemMatch: {a:2, b:2}}]}}
//    -> foo.a: [1, 1]
// Or -> foo.a: [2, 2]
TEST(CMCollapseTreeTest, BasicAllElemMatch) {
    Interval expectedInterval(BSON("" << 1 << "" << 1), true, true);

    const char* queryStr = "{foo: {$all: [ {$elemMatch: {a:1, b:1}} ]}}";
    unique_ptr<CanonicalQuery> query(canonicalize(queryStr));
    ASSERT(query.get() != NULL);

    BSONObj key = fromjson("{'foo.a': 1}");

    IndexBounds indexBounds = ChunkManager::getIndexBoundsForQuery(key, *query.get());
    ASSERT_EQUALS(indexBounds.size(), 1U);
    const OrderedIntervalList& oil = indexBounds.fields.front();
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    const Interval& interval = oil.intervals.front();

    // Choose one of the two possible solutions.
    // Two solutions differ only by assignment of index tags.
    ASSERT(Interval::INTERVAL_EQUALS == interval.compare(expectedInterval));
}

// {a : [1, 2, 3]} -> a: [1, 1], [[1, 2, 3], [1, 2, 3]]
TEST(CMCollapseTreeTest, ArrayEquality) {
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
TEST(CMCollapseTreeTest, Regex) {
    OrderedIntervalList expected;
    expected.intervals.push_back(Interval(BSON(""
                                               << ""
                                               << ""
                                               << BSONObj()),
                                          true,
                                          false));
    BSONObjBuilder builder;
    builder.appendRegex("", "abc");
    builder.appendRegex("", "abc");
    expected.intervals.push_back(Interval(builder.obj(), true, true));
    checkIndexBounds("{ a: /abc/ }", expected);
}

// {$where: 'this.credits == this.debits' }
TEST(CMCollapseTreeTest, Where) {
    OrderedIntervalList expected;
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expected.intervals.push_back(Interval(builder.obj(), true, true));
    checkIndexBounds("{$where: 'this.credits == this.debits' }", expected);
}

// { $text: { $search: "coffee -cake" } }
TEST(CMCollapseTreeTest, Text) {
    OrderedIntervalList expected;
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expected.intervals.push_back(Interval(builder.obj(), true, true));
    checkIndexBounds("{ $text: { $search: 'coffee -cake' } }", expected);
}

// { a: 2, $text: { $search: "leche", $language: "es" } }
TEST(CMCollapseTreeTest, TextWithQuery) {
    OrderedIntervalList expected;
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expected.intervals.push_back(Interval(builder.obj(), true, true));
    checkIndexBounds("{ a: 2, $text: { $search: 'leche', $language: 'es' } }", expected);
}

//  { a: 0 } -> hashed a: [hash(0), hash(0)]
TEST(CMCollapseTreeTest, HashedSinglePoint) {
    const char* queryStr = "{ a: 0 }";
    unique_ptr<CanonicalQuery> query(canonicalize(queryStr));
    ASSERT(query.get() != NULL);

    BSONObj key = fromjson("{a: 'hashed'}");

    IndexBounds indexBounds = ChunkManager::getIndexBoundsForQuery(key, *query.get());
    ASSERT_EQUALS(indexBounds.size(), 1U);
    const OrderedIntervalList& oil = indexBounds.fields.front();
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    const Interval& interval = oil.intervals.front();
    ASSERT(interval.isPoint());
}

// { a: { $lt: 2, $gt: 1} } -> hashed a: [Minkey, Maxkey]
TEST(CMCollapseTreeTest, HashedRange) {
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
TEST(CMCollapseTreeTest, HashedRegex) {
    IndexBounds expectedBounds;
    expectedBounds.fields.push_back(OrderedIntervalList());
    OrderedIntervalList& expectedOil = expectedBounds.fields.front();
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    expectedOil.intervals.push_back(Interval(builder.obj(), true, true));

    checkIndexBoundsWithKey("{a: 'hashed'}", "{ a: /abc/ }", expectedBounds);
}

/**
 * KeyPattern key bounds generation test
 */

void CheckBoundList(const BoundList& list, const BoundList& expected) {
    ASSERT_EQUALS(list.size(), expected.size());
    for (size_t i = 0; i < list.size(); i++) {
        ASSERT_EQUALS(list[i].first.woCompare(expected[i].first), 0);
        ASSERT_EQUALS(list[i].second.woCompare(expected[i].second), 0);
    }
}

// Key { a: 1 }, Bounds a: [0]
//  => { a: 0 } -> { a: 0 }
TEST(CMKeyBoundsTest, Basic) {
    IndexBounds indexBounds;
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.front().intervals.push_back(Interval(BSON("" << 0 << "" << 0), true, true));

    BoundList expectedList;
    expectedList.push_back(make_pair(fromjson("{a: 0}"), fromjson("{a: 0}")));

    ShardKeyPattern skeyPattern(fromjson("{a: 1}"));
    BoundList list = skeyPattern.flattenBounds(indexBounds);
    CheckBoundList(list, expectedList);
}

// Key { a: 1 }, Bounds a: [2, 3)
//  => { a: 2 } -> { a: 3 }  // bound inclusion is ignored.
TEST(CMKeyBoundsTest, SingleInterval) {
    IndexBounds indexBounds;
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.front().intervals.push_back(Interval(BSON("" << 2 << "" << 3), true, false));

    BoundList expectedList;
    expectedList.push_back(make_pair(fromjson("{a: 2}"), fromjson("{a: 3}")));

    ShardKeyPattern skeyPattern(fromjson("{a: 1}"));
    BoundList list = skeyPattern.flattenBounds(indexBounds);
    CheckBoundList(list, expectedList);
}

// Key { a: 1, b: 1, c: 1 }, Bounds a: [2, 3), b: [2, 3), c: [2: 3)
//  => { a: 2, b: 2, c: 2 } -> { a: 3, b: 3, c: 3 }
TEST(CMKeyBoundsTest, MultiIntervals) {
    IndexBounds indexBounds;
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields.push_back(OrderedIntervalList());
    indexBounds.fields[0].intervals.push_back(Interval(BSON("" << 2 << "" << 3), true, false));
    indexBounds.fields[1].intervals.push_back(Interval(BSON("" << 2 << "" << 3), true, false));
    indexBounds.fields[2].intervals.push_back(Interval(BSON("" << 2 << "" << 3), true, false));

    BoundList expectedList;
    expectedList.push_back(
        make_pair(fromjson("{ a: 2, b: 2, c: 2 }"), fromjson("{ a: 3, b: 3, c: 3 }")));

    ShardKeyPattern skeyPattern(fromjson("{a: 1, b: 1, c: 1}"));
    BoundList list = skeyPattern.flattenBounds(indexBounds);
    CheckBoundList(list, expectedList);
}

// Key { a: 1, b: 1, c: 1 }, Bounds a: [0, 0], b: { $in: [4, 5, 6] }, c: [2: 3)
//  => { a: 0, b: 4, c: 2 } -> { a: 0, b: 4, c: 3 }
//     { a: 0, b: 5, c: 2 } -> { a: 0, b: 5, c: 3 }
//     { a: 0, b: 6, c: 2 } -> { a: 0, b: 6, c: 3 }
TEST(CMKeyBoundsTest, IntervalExpansion) {
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
    expectedList.push_back(
        make_pair(fromjson("{ a: 0, b: 4, c: 2 }"), fromjson("{ a: 0, b: 4, c: 3 }")));
    expectedList.push_back(
        make_pair(fromjson("{ a: 0, b: 5, c: 2 }"), fromjson("{ a: 0, b: 5, c: 3 }")));
    expectedList.push_back(
        make_pair(fromjson("{ a: 0, b: 6, c: 2 }"), fromjson("{ a: 0, b: 6, c: 3 }")));

    ShardKeyPattern skeyPattern(fromjson("{a: 1, b: 1, c: 1}"));
    BoundList list = skeyPattern.flattenBounds(indexBounds);
    CheckBoundList(list, expectedList);
}

// Key { a: 1, b: 1, c: 1 }, Bounds a: [0, 1], b: { $in: [4, 5, 6] }, c: [2: 3)
//  => { a: 0, b: 4, c: 2 } -> { a: 1, b: 6, c: 3 }
// Since field "a" is not a point, expasion after "a" is not allowed.
TEST(CMKeyBoundsTest, NonPointIntervalExpasion) {
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
    expectedList.push_back(
        make_pair(fromjson("{ a: 0, b: 4, c: 2 }"), fromjson("{ a: 1, b: 6, c: 3 }")));

    ShardKeyPattern skeyPattern(fromjson("{a: 1, b: 1, c: 1}"));
    BoundList list = skeyPattern.flattenBounds(indexBounds);
    CheckBoundList(list, expectedList);
}

}  // namespace
