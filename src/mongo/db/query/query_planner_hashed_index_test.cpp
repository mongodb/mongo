/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/hasher.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/str.h"

namespace mongo {
/**
 * A specialization of the QueryPlannerTest fixture which makes it easy to present the planner
 * with a view of the available hashed indexes.
 */
class QueryPlannerHashedTest : public QueryPlannerTest {
protected:
    void setUp() final {
        QueryPlannerTest::setUp();

        // We're interested in testing plans that use a hashed index, so don't generate collection
        // scans.
        params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    }

    /**
     * Returns string of the format "[<hash of 'value'>,<hash of 'value'>,true,true]".
     */
    template <class T>
    std::string getHashedBound(T value) {
        auto hashedValue = BSONElementHasher::hash64(BSON("" << value).firstElement(), 0);
        return str::stream() << "[ " << hashedValue << ", " << hashedValue << ", true, true]";
    }
};

//
// Range queries.
//
TEST_F(QueryPlannerHashedTest, RangeQueryWhenHashedFieldIsAPrefix) {
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1 << "z" << -1));

    // Range query on hashed field cannot use index.
    runQuery(fromjson("{'x': {$gt: 0, $lt: 9}}"));
    assertHasOnlyCollscan();

    // Range query on hashed field  cannot use index even if range would lead to empty bounds.
    runQuery(fromjson("{x: {$lte: 5, $gte: 10}}"));
    assertHasOnlyCollscan();

    // Range query on non-hashed field can use index, if there is an equality match on the hashed
    // prefix.
    runQuery(fromjson("{x: 2, y: {$gt: 0, $lte: 9}}"));

    // Verify that fetch stage only has a filter on hashed field and correct bounds are used.
    assertSolutionExists(
        "{fetch: {filter: {x: 2}, node: {ixscan:{pattern: {x: 'hashed', y: 1, z: -1}, bounds: "
        "{x: [" +
        getHashedBound(2) + "], y: [[0,9,false,true]], z: [['MaxKey','MinKey',true,true]] }}}}}");
}

TEST_F(QueryPlannerHashedTest, RangeQueryWhenNonHashedFieldIsAPrefix) {
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"
                      << "z" << -1));

    // Range query on non-hashed field can use index.
    runQuery(fromjson("{x: {$gte: 0, $lt: 9}}"));

    // Verify that fetch stage has no filter and correct bounds are used.
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan:{pattern: {x: 1, y: 'hashed', z: "
        "-1}, bounds: {x: [[0,9,true,false]],y: [['MinKey','MaxKey',true,true]], z: "
        "[['MaxKey','MinKey',true,true]] }}}}}");

    // Range query on hashed field cannot use index for hashed component.
    runQuery(fromjson("{x: {$gte: 0, $lt: 9}, y: {$lte: 5, $gte: 10}}"));

    // Verify that fetch stage only has a filter on hashed field and validate the bounds.
    assertSolutionExists(
        "{fetch: {filter: {y: {$lte: 5, $gte: 10}}, node: {ixscan:{pattern: {x: 1, y: "
        "'hashed', z: -1}, bounds: {x: [[0,9,true,false]],y: [['MinKey','MaxKey',true,true]], z: "
        "[['MaxKey','MinKey',true,true]] }}}}}");
}

//
// Tests related to object comparison.
//
TEST_F(QueryPlannerHashedTest, DottedFieldInIndex) {
    addIndex(BSON("x.a"
                  << "hashed"
                  << "y" << 1 << "z" << -1));

    // Cannot use index when the query is on a prefix of the index field.
    runQuery(fromjson("{x: {a: 1}}"));
    assertHasOnlyCollscan();

    // Can use index when query is on the index field.
    runQuery(fromjson("{'x.a' : 1}"));
    assertSolutionExists(
        "{fetch: {filter: {'x.a': 1}, node: {ixscan:{pattern: {'x.a': 'hashed', y: "
        "1, z: -1}, bounds: {'x.a': [" +
        getHashedBound(1) +
        "], y: [['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]] }}}}}");
}

TEST_F(QueryPlannerHashedTest, QueryOnObject) {
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1 << "z" << -1));

    // Can use index when the query predicate is an object.
    runQuery(fromjson("{x: {a: 1}, y: 1}"));
    assertSolutionExists(
        "{fetch: {filter: {x: {$eq : {a: 1}}}, node: {ixscan:{pattern: {'x': 'hashed', y: "
        "1, z: -1}, bounds: {'x': [" +
        getHashedBound(fromjson("{a: 1}")) +
        "], y: [[1,1,true,true]], z: [['MaxKey','MinKey',true,true]] }}}}}");

    // Cannot use index to query on a sub-object on an index field.
    runQuery(fromjson("{'x.a' : 1}"));
    assertHasOnlyCollscan();
}

//
// Null comparison and existence tests for compound hashed index.
//
TEST_F(QueryPlannerHashedTest, ExistsTrueQueries) {
    // $exists:true query can use index regardless of prefix field type.
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1 << "z" << -1));
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"
                      << "z" << -1));
    runQuery(fromjson("{x: {$exists: true}, y: {$exists: true}, z: {$exists: true}}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {x: {$exists: true}, y: {$exists: true}, z: {$exists: true}}, node: "
        "{ixscan: {pattern: {x: 'hashed', y: 1, z:-1}, bounds: {x: "
        "[['MinKey','MaxKey',true,true]],y: [['MinKey','MaxKey',true,true]], z: "
        "[['MaxKey','MinKey',true,true]] }}}}}");
    assertSolutionExists(
        "{fetch: {filter: {x: {$exists: true}, y: {$exists: true}, z: {$exists: true}}, node: "
        "{ixscan: {pattern: {x: 1, y: 'hashed', z:-1}, bounds: {x: "
        "[['MinKey','MaxKey',true,true]],y: [['MinKey','MaxKey',true,true]], z: "
        "[['MaxKey','MinKey',true,true]] }}}}}");
}

TEST_F(QueryPlannerHashedTest, ExistsFalseQueries) {
    // $exists:false query can use index regardless of prefix field type.
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1 << "z" << -1));
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"
                      << "z" << -1));
    runQuery(fromjson("{x: {$exists: false}, y: {$exists: false}, z: {$exists: false}}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {x: {$exists: false}, y: {$exists: false}, z: {$exists: false}}, node: "
        "{ixscan: {pattern: {x: 'hashed', y: 1, z:-1}, bounds: {x: [" +
        getHashedBound(BSONNULL) +
        "], y: [[null, null, true, true]], z: [[null, null, true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {x: {$exists: false}, y: {$exists: false}, z: {$exists: false}}, node: "
        "{ixscan: {pattern: {x: 1, y: 'hashed', z:-1}, bounds: {x: [[null, null, true, true]], y: "
        "[" +
        getHashedBound(BSONNULL) + "], z: [[null, null, true, true]]}}}}}");
}

TEST_F(QueryPlannerHashedTest, NegationQueriesOnHashedPrefix) {
    // $not queries on a hashed prefix field cannot use index.
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1 << "z" << -1));
    runQuery(fromjson("{x: {$ne: null}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{x: {$not: {$eq: 5}}}"));
    assertHasOnlyCollscan();

    // $not queries on non-hashed fields of a hashed-prefix index can use index.
    runQuery(fromjson("{x: null, y: {$nin: [1, 2]}, z: {$ne: null}}"));
    assertSolutionExists(
        "{fetch: {filter: {x: {$eq: null}}, node: {ixscan: {pattern: {x: 'hashed', y: 1, z: -1}, "
        "bounds: {x: [" +
        getHashedBound(BSONUndefined) + "," + getHashedBound(BSONNULL) +
        "], y: [['MinKey', 1, true, false], [1, 2, false, false], [2,'MaxKey', false, true]], z: "
        "[['MaxKey', null, true, false], [undefined, 'MinKey', false, true]]}}}}}");
}

TEST_F(QueryPlannerHashedTest, NegationQueriesOnHashedNonPrefix) {
    // $not queries on a non-hashed prefix field can use the compound hashed index. A negated
    // predicate on the hashed field will produce bounds of [MinKey, MaxKey].
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"
                      << "z" << -1));

    runQuery(fromjson("{x: {$nin: [1, 2]}, y: {$not: {$eq: null}}, z: {$ne: null}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{fetch: {filter: {y: {$ne: null}}, node: {ixscan: {pattern: {x: 1, y: 'hashed', z: -1}, "
        "bounds: {x: [['MinKey', 1, true, false], [1, 2, false, false], [2,'MaxKey', false, "
        "true]], y: [['MinKey', 'MaxKey', true, true]], z: [['MaxKey', null, true, false], "
        "[undefined, 'MinKey', false, true]]}}}}}");

    runQuery(fromjson("{x: {$nin: [1, 2]}, y: {$ne: 5}, z: {$ne: null}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{fetch: {filter: {y: {$ne: 5}}, node: {ixscan: {pattern: {x: 1, y: 'hashed', z: -1}, "
        "bounds: {x: [['MinKey', 1, true, false], [1, 2, false, false], [2,'MaxKey', false, "
        "true]], y: [['MinKey', 'MaxKey', true, true]], z: [['MaxKey', null, true, false], "
        "[undefined, 'MinKey', false, true]]}}}}}");
}

TEST_F(QueryPlannerHashedTest, EqualsNullQueries) {
    // When hashed field is a prefix, $eq:null queries can use index. The bounds will be point
    // intervals of hashed value of 'undefined' and 'null'.
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1 << "z" << -1));
    runQuery(fromjson("{x: {$eq: null}}"));
    assertSolutionExists(
        "{fetch: {filter: {x: {$eq: null}}, node: {ixscan:{pattern: {x: 'hashed', y: 1, z: "
        "-1},bounds: {x: [" +
        getHashedBound(BSONUndefined) + "," + getHashedBound(BSONNULL) +
        "],y: [['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]]"
        "}}}}}");

    // $eq:null queries on a non-hashed field can use the index. The bounds will be point intervals
    // of 'undefined' and 'null'.
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"
                      << "z" << -1));
    runQuery(fromjson("{x: {$eq: null}}"));
    assertSolutionExists(
        "{fetch: {filter: {x: {$eq: null}}, node: {ixscan:{pattern: {x: 1, y: 'hashed', z: -1}, "
        "bounds: {x:[[undefined, undefined, true, true],[null, null, true, true]], y: "
        "[['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]]"
        "}}}}}");
}

//
// Tests with $or operator.
//
TEST_F(QueryPlannerHashedTest, OrWithMultipleEqualityPredicatesOnHashedPrefixUsesSingleIndexScan) {
    addIndex(BSON("a"
                  << "hashed"
                  << "b" << 1));
    runQuery(fromjson("{$or: [{a: 5}, {a: 10}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$in: [5, 10]}}, node: {ixscan:{pattern: {a: 'hashed', b:1}, "
        "bounds: "
        "{a: [" +
        getHashedBound(5) + "," + getHashedBound(10) +
        + "], "
        " b: [['MinKey','MaxKey',true,true]] }}}}}");
}

TEST_F(QueryPlannerHashedTest,
       OrWithMultipleEqualityPredicatesOnNonHashedPrefixUsesSingleIndexScan) {
    addIndex(BSON("a" << 1 << "b"
                      << "hashed"));
    runQuery(fromjson("{$or: [{a: 5}, {a: 10}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: 1, b: 'hashed'}, "
        "bounds: {a:[[5,5,true,true],[10,10,true,true]], b: [['MinKey','MaxKey',true,true]] }}}}}");
}

TEST_F(QueryPlannerHashedTest, OrWithOneRegularAndOneHashedIndexPathUsesTwoIndexes) {
    addIndex(BSON("a"
                  << "hashed"
                  << "c" << 1));
    addIndex(BSON("b" << 1));
    runQuery(fromjson("{$or: [{a: 5}, {b: 10}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {$or: [{a: 5}]}, node: {ixscan: {filter: null, pattern: "
        "{a: 'hashed', c:1}, bounds: {a: [" +
        getHashedBound(5) +
        "]}}}}}, {ixscan: {filter: null, pattern: {b: 1},"
        "bounds: {b: [[10,10,true,true]]}}}]}}}}");
}

TEST_F(QueryPlannerHashedTest, OrPushdownWithHashedPrefix) {
    addIndex(BSON("a"
                  << "hashed"
                  << "b" << 1));
    addIndex(BSON("a"
                  << "hashed"
                  << "c" << 1));
    runQuery(fromjson("{a: 1, $or: [{b: 2}, {c: 3}]}"));
    assertNumSolutions(3U);

    // Verify that three different solution, with index1, index2 and a union of index1 solution,
    // index2 solution are generated.
    assertSolutionExists(
        "{fetch: {filter: {$and : [{a: {$eq: 1}}, {$or: [{b: {$eq: 2}}, {c: {$eq: 3}}]}]}, node: "
        "{ixscan: {pattern: {a: 'hashed', b: 1}, filter: null, bounds: {a: [" +
        getHashedBound(1) + "], b: [['MinKey','MaxKey',true,true]]}}}}}");

    assertSolutionExists(
        "{fetch: { filter: {$and : [{a: {$eq: 1}}, {$or: [{b: {$eq: 2}}, {c: {$eq: 3}}]}]}, node: "
        "{ixscan: {pattern: {a: 'hashed', c: 1}, filter: null, bounds: {a: [" +
        getHashedBound(1) + "], c: [['MinKey','MaxKey',true,true]]}}}}}");

    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: { filter: {a: {$eq: 1}}, node: {ixscan: {pattern: {a: 'hashed', b: "
        "1}, filter: null, bounds: {a: [" +
        getHashedBound(1) +
        "], b: [[2,2,true,true]]}}}}},"  // First sub-plan end.
        "{fetch: { filter: {a: {$eq: 1}}, node: {ixscan: {pattern: {a: 'hashed', c: 1}, filter: "
        "null, bounds: {a: [" +
        getHashedBound(1) +
        "], c: [[3,3,true,true]]}}}}}"  // Second sub-plan end.
        "]}}");
}

TEST_F(QueryPlannerHashedTest, OrPushdownWithNonHashedPrefix) {
    addIndex(BSON("a" << 1 << "b"
                      << "hashed"));
    addIndex(BSON("a"
                  << "hashed"
                  << "c" << 1));
    runQuery(fromjson("{a: 1, $or: [{b: 2}, {c: 3}]}"));
    assertNumSolutions(3U);

    // Verify that three different solution, with index1, index2 and a union of index1 solution,
    // index2 solution are generated.
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: {$eq: 2}}, {c: {$eq: 3}}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 'hashed'}, filter: null, bounds: {a: [[1,1,true,true]], b: "
        "[['MinKey','MaxKey',true,true]]}}}}}");

    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$eq: 1}}, {$or: [{b: {$eq: 2}}, {c: {$eq: 3}}]}]}, node: "
        "{ixscan: {pattern: {a: 'hashed', c: 1}, filter: null, bounds: {a: [" +
        getHashedBound(1) + "], c: [['MinKey','MaxKey',true,true]]}}}}}");

    assertSolutionExists(
        "{or: {nodes: [{fetch: {filter: {b: {$eq: 2}}, node: "
        "{ixscan: {pattern: {a: 1, b: 'hashed'}, filter: null, bounds: {a: [[1,1,true,true]], "
        "b: [" +
        getHashedBound(2) + "]}}}}}, " +
        "{fetch: {filter: {a: {$eq: 1}}, node: {ixscan: {pattern: {a: 'hashed', c: 1}, filter: "
        "null, bounds: {a: [" +
        getHashedBound(1) + "], c: [[3,3,true,true]]}}}}}]}}");
}

//
// Covered projections.
//
TEST_F(QueryPlannerHashedTest, CannotCoverQueryWhenHashedFieldIsPrefix) {
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1));

    // Verify that a query doesn't get covered when the query is on a hashed field, even if the
    // projection doesn't include the hashed field. This is to avoid the possibility of hash
    // collision. If two different fields produce the same hash value, there is no way to
    // distinguish them without fetching the document.
    runQueryAsCommand(
        fromjson("{find: 'test', filter: {x : {$eq: 5}}, projection:{_id: 0, y: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, y: 1}, node: {fetch: {filter: {x : {$eq: 5}}, node: {ixscan: "
        "{filter: null, pattern: {x: 'hashed', y: 1}, bounds: {x:[" +
        getHashedBound(5) + "], y: [['MinKey','MaxKey',true,true]] }}}}}}}");

    // Verify that queries cannot be covered with hashed field is a prefix, despite query and
    // projection not using hashed fields.
    runQueryAsCommand(
        fromjson("{find: 'test', filter: {y : {$eq: 5}}, projection:{_id: 0, y: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{proj: {spec: {_id: 0, y: 1}, node: {cscan: {dir: 1}} }}");
}

TEST_F(QueryPlannerHashedTest, CanCoverQueryWhenHashedFieldIsNotPrefix) {
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"
                      << "z" << -1));

    // Verify that query gets covered when neither query nor project use hashed field.
    runQueryAsCommand(
        fromjson("{find: 'test', filter: {x: {$gt: 24, $lt: 27}}, projection:{_id: 0, z: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, z: 1}, node: {ixscan: {filter: null, pattern: {x: 1, y: 'hashed', "
        "z: -1}, bounds: {x: [[24,27,false,false]], y: [['MinKey','MaxKey',true,true]], z: "
        "[['MaxKey','MinKey',true,true]] }}}}}");

    // Verify that query doesn't get covered when query is on a hashed field.
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 1, y: 1}, projection:{_id: 0, z: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, z: 1}, node: {fetch: {filter: {y : {$eq: 1}}, node: {ixscan: "
        "{filter: null, pattern: {x: 1, y: 'hashed', z: -1}, bounds: {x: [[1,1,true,true]], y:[" +
        getHashedBound(1) + "] , z: [['MaxKey','MinKey',true,true]]} }} }} }}");

    // Verify that the query doesn't get covered when projection is on a hashed field.
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 1}, projection:{_id: 0, y: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, y: 1}, node: {fetch: {filter: null, node: {ixscan: {filter: null, "
        "pattern: {x: 1, y: 'hashed', z: -1}, bounds: {x: [[1,1,true,true]], y: "
        "[['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]]} }} }} }}");
}

//
// Tests with 'INCLUDE_SHARD_FILTER' parameter set.
//
TEST_F(QueryPlannerHashedTest, CompoundHashedShardKeyWhenIndexAndShardKeyBothProvideHashedField) {
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"
                      << "z" << -1));
    params.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("x" << 1 << "y"
                               << "hashed");

    // Verify that query gets covered when neither query nor project use hashed field.
    runQueryAsCommand(
        fromjson("{find: 'test', filter: {x: {$gt: 24, $lt: 27}}, projection:{_id: 0, z: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, z: 1}, node: {sharding_filter: {node: {ixscan: {filter: null, "
        "pattern: {x: 1, y: 'hashed', z: -1}, bounds: {x: [[24,27,false,false]], y: "
        "[['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]]} }} }} }}");
}
TEST_F(QueryPlannerHashedTest,
       CompoundHashedShardKeyWhenIndexAndShardKeyBothHashedWithQueryOnHashedField) {
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"
                      << "z" << -1));
    params.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("x" << 1 << "y"
                               << "hashed");

    // Verify that the query doesn't get covered when projection is on a hashed field.
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 1}, projection:{_id: 0, y: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, y: 1}, node: {fetch: {filter: null, node: {sharding_filter: {node: "
        " {ixscan: {filter: null, pattern: {x: 1, y: 'hashed', z: -1},bounds: {x: "
        "[[1,1,true,true]], y: [['MinKey','MaxKey',true,true]], z: "
        "[['MaxKey','MinKey',true,true]]} }} }} }} }}");

    // Verify that query doesn't get covered when query is on a hashed field, even though the
    // projection does not include the hashed field.
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 1, y: 1}, projection:{_id: 0, z: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, z: 1}, node: {sharding_filter: {node: {fetch: {filter: {y : {$eq: "
        "1}}, node: {ixscan: {filter: null, pattern: {x: 1, y: 'hashed', z: -1},bounds: {x: "
        "[[1,1,true,true]], y:[" +
        getHashedBound(1) + "] , z: [['MaxKey','MinKey',true,true]]} }} }} }} }}");
}

TEST_F(QueryPlannerHashedTest,
       CompoundHashedShardKeyWhenIndexProvidesNonHashedAndShardKeyIsHashed) {
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"
                      << "z" << -1));
    params.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;

    // Can cover the query when index provides range value for a field ('z'), but the corresponding
    // shard key field is hashed.
    params.shardKey = BSON("x" << 1 << "z"
                               << "hashed");
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 1}, projection:{_id: 0, z: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, z: 1}, node: {sharding_filter: {node: {ixscan: {filter: null, "
        "pattern: {x: 1, y: 'hashed', z: -1}, bounds: {x: [[1,1,true,true]], y: "
        "[['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]]} }} }} }}");
}
TEST_F(QueryPlannerHashedTest,
       CompoundHashedShardKeyWhenIndexProvidesHashedAndShardKeyIsNonHashed) {
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"
                      << "z" << -1));
    params.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;

    // Cannot cover the query when index provides hashed value for a field ('y'), but the
    // corresponding shard key field is a range field.
    params.shardKey = BSON("x" << 1 << "y" << 1);
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 1}, projection:{_id: 0, x: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {sharding_filter: {node: {fetch: {filter: null, node: "
        " {ixscan: "
        "{filter: null, pattern: {x: 1, y: 'hashed', z: -1},"
        "bounds: {x: [[1,1,true,true]], y: [['MinKey','MaxKey',true,true]], z: "
        "[['MaxKey','MinKey',true,true]]} }} }} }} }}");
}

TEST_F(QueryPlannerHashedTest, CompoundHashedShardKeyWhenIndexDoesNotHaveAllShardKeyFields) {
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"
                      << "z" << -1));
    params.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;

    // Cannot cover the query when one of the shard key field ('newField') is not in the index.
    params.shardKey = BSON("x" << 1 << "y"
                               << "hashed"
                               << "newField" << 1);
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 1}, projection:{_id: 0, x: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {sharding_filter: {node: {fetch: {filter: null, node: "
        " {ixscan: {filter: null, pattern: {x: 1, y: 'hashed', z: -1}, bounds: {x: "
        "[[1,1,true,true]], y: [['MinKey','MaxKey',true,true]], z: "
        "[['MaxKey','MinKey',true,true]]} }} }} }} }}");

    params.shardKey = BSON("x" << 1 << "newField"
                               << "hashed");
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 1}, projection:{_id: 0, x: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {sharding_filter: {node: {fetch: {filter: null, node: "
        " {ixscan: {filter: null, pattern: {x: 1, y: 'hashed', z: -1}, bounds: {x: "
        "[[1,1,true,true]], y: [['MinKey','MaxKey',true,true]], z: "
        "[['MaxKey','MinKey',true,true]]} }} }} }} }}");
}

//
// Sorting tests.
//
TEST_F(QueryPlannerHashedTest, SortWhenHashedFieldIsPrefix) {
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << -1 << "z" << 1));

    // Verify that sort on a hashed field results in collection scan.
    runQueryAsCommand(fromjson("{find: 'test', filter: {}, sort: {x: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{sort: {pattern: {x: 1}, limit: 0, node: {cscan: {dir: 1}} }}");

    // Verify that a list of exact match predicates on hashed field (prefix) and sort with an
    // immediate range field can use 'SORT_MERGE'.
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: {$in: [1, 2]}}, sort: {y: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {x: {$in: [1,2]}}, node: {mergeSort: {nodes: [{ixscan: {pattern: {x: "
        "'hashed', y: -1, z: 1}, bounds: {x: [" +
        getHashedBound(1) +
        "], y: [['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]]}}},"
        "{ixscan: {pattern: {x: 'hashed', y: -1, z: 1}, bounds: {x: [" +
        getHashedBound(2) +
        "], y: [['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]]}}}]}}}}");

    // Verify that an equality predicate on hashed field (prefix) and sort with an immediate
    // range field can be sorted by the index.
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 1}, sort: {y: 1, z: -1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {x: {$eq: 1}}, node: {ixscan: {pattern: {x: "
        "'hashed', y: -1, z: 1}, dir: -1, bounds: {x: [" +
        getHashedBound(1) +
        "], y: [['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]] } }} }}");

    // {$exists: false} is treated as a point-interval in BSONNULL. Hence index can provide the
    // sort.
    runQueryAsCommand(
        fromjson("{find: 'test', filter: {x: {$exists: false}}, sort: {y: 1, z: -1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {x: {$exists: false}}, node: {ixscan: {pattern: {x: 'hashed', y: -1, z: "
        "1}, dir: -1, bounds: {x: [" +
        getHashedBound(BSONNULL) +
        "], y: [['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]] } }} }}");

    // Sort on any index field other than the one immediately following the hashed field will use a
    // blocking sort.
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 3}, sort: {z: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {z:1}, limit: 0, type: 'simple', node: {fetch: {filter: {x: {$eq: "
        "3}}, node: {ixscan: {pattern: {x: 'hashed', y: -1, z: 1}, bounds: {x: [" +
        getHashedBound(3) +
        "], y: [['MaxKey','MinKey',true,true]], z: [['MinKey','MaxKey',true,true]]} }} }} }}");
}

TEST_F(QueryPlannerHashedTest, SortWhenNonHashedFieldIsPrefix) {
    addIndex(BSON("x" << 1 << "y" << -1 << "z"
                      << "hashed"
                      << "a" << 1));

    // Verify that sort on a hashed field results in collection scan.
    runQueryAsCommand(fromjson("{find: 'test', filter: {}, sort: {x: 1, y: -1, z: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {x: 1, y: -1, z: 1}, limit: 0, node: {cscan: {dir: 1}} }}");

    // Verify that a list of exact match predicates on range field (prefix) and sort with an
    // immediate range field can use 'SORT_MERGE'.
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: {$in: [1, 2]}}, sort: {y: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {mergeSort: {nodes: [{ixscan: {pattern: {x: "
        "1, y: -1, z: 'hashed', a: 1}, bounds: {x: [[2,2,true,true]], y: "
        "[['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]], a: "
        "[['MaxKey','MinKey',true,true]]}}},"
        "{ixscan: {pattern: {x: 1, y: -1, z: 'hashed', a: 1}, bounds: {x: [[1,1,true,true]], y: "
        "[['MinKey','MaxKey',true,true]], z: [['MaxKey','MinKey',true,true]], a: "
        "[['MaxKey','MinKey',true,true]]}}}]}}}}");

    // Verify that an exact match predicate on range field (prefix) and sort with an immediate range
    // field doesn't require any additional sort stages. The entire operation can be answered by the
    // index.
    runQueryAsCommand(
        fromjson("{find: 'test', filter: {x: 1}, sort: {y: -1}, projection: {_id: 0, a: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {ixscan: {pattern: {x: "
        "1, y: -1, z: 'hashed', a: 1}, dir: 1, bounds: {x: [[1,1,true,true]], y: "
        "[['MaxKey','MinKey',true,true]], z: [['MinKey','MaxKey',true,true]], a: "
        "[['MinKey','MaxKey',true,true]]}}}}}");

    // Verify that query predicate and sort on non-hashed fields can be answered without fetching
    // the document, but require a sort stage, if the 'sort' field is not immediately after 'query'
    // field in the index.
    runQueryAsCommand(
        fromjson("{find: 'test', filter: {x: 3}, sort: {a: 1}, projection: {_id: 0, y: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, y: 1}, node: "
        "{sort: {pattern: {a: 1}, limit: 0, type: 'default', node: "
        "{ixscan: {pattern: {x: 1, y: -1, z: 'hashed', a: 1}, bounds: {x: "
        "[[3,3,true,true]], y: [['MaxKey','MinKey',true,true]], z: "
        "[['MinKey','MaxKey',true,true]], a: [['MinKey','MaxKey',true,true]]} }} }} }}");

    //  Verify that sort on a hashed field requires a fetch and a blocking sort stage.
    runQueryAsCommand(
        fromjson("{find: 'test', filter: {x: 3}, sort: {z: 1}, projection: {_id: 0, y: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, y: 1}, node: "
        "{sort: {pattern: {z:1}, limit: 0, type: 'simple', node: "
        "{fetch: {filter: null, node: {ixscan: {pattern: {x: 1, y: -1, z: 'hashed', a: 1}, "
        "bounds: {x: [[3,3,true,true]], y: [['MaxKey','MinKey',true,true]], z: "
        "[['MinKey','MaxKey',true,true]], a: [['MinKey','MaxKey',true,true]]} }} }} }} }}");
}

TEST_F(QueryPlannerHashedTest, SortWithMissingOrIrrelevantQueryPredicate) {
    addIndex(BSON("x" << 1 << "y" << -1 << "z"
                      << "hashed"
                      << "a" << 1));

    // Verify that a sort on non-hashed fields doesn't require any additional sort stages. The
    // entire operation can be answered by the index. Also verify that if the projection only
    // includes non-hashed index fields, plan does not use a fetch stage.
    runQueryAsCommand(fromjson(
        "{find: 'test', filter: {}, sort: {x: 1, y: -1}, projection: {_id: 0, x: 1, y: 1, a: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: 1, y: 1, a: 1}, node: {ixscan: {pattern: {x: 1, y: -1, z: "
        "'hashed', a: 1}, bounds: {x: [['MinKey','MaxKey',true,true]], y: "
        "[['MaxKey','MinKey',true,true]], z: [['MinKey','MaxKey',true,true]], a: "
        "[['MinKey','MaxKey',true,true]]} }} }}");

    // Verify that a sort on non-hashed fields with a query predicate on fields irrelevant to the
    // index, doesn't require any additional sort stages.
    runQueryAsCommand(fromjson("{find: 'test', filter: {p: 5}, sort: {x: 1, y: -1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {p: {$eq: 5}}, node: {ixscan: {pattern: {x: 1, y: -1, z: 'hashed', a: "
        "1}, bounds: {x: [['MinKey','MaxKey',true,true]], y: [['MaxKey','MinKey',true,true]], z: "
        "[['MinKey','MaxKey',true,true]], a: [['MinKey','MaxKey',true,true]]} }} }}");

    // Verify that a sort on non-hashed fields doesn't require any additional sort stages. The
    // entire operation can be answered by the index. Also verify that if the projection includes
    // hashed fields, the operation will require a fetch stage.
    runQueryAsCommand(fromjson(
        "{find: 'test', filter: {}, sort: {x: 1, y: -1}, projection: {_id: 0, x: 1, z: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: 1, z: 1}, node: {fetch: {filter: null, node: {ixscan: "
        "{pattern: {x: 1, y: -1, z: 'hashed', a: 1}, bounds: {x: [['MinKey','MaxKey',true,true]], "
        "y: [['MaxKey','MinKey',true,true]], z: [['MinKey','MaxKey',true,true]], a: "
        "[['MinKey','MaxKey',true,true]]} }} }} }}");
}

//
// Partial index tests.
//
TEST_F(QueryPlannerHashedTest, PartialIndexCanAnswerPredicateOnFilteredFieldWithHashedPrefix) {
    auto filterObj = fromjson("{x: {$gt: 0}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1),
             filterExpr.get());

    runQuery(fromjson("{x: 5}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {x : {$eq: 5}}, node: "
        "{ixscan: {filter: null, pattern: {x: 'hashed', y: 1},"
        "bounds: {x:[" +
        getHashedBound(5) + "], y: [['MinKey','MaxKey',true,true]] }}}}}");

    runQuery(fromjson("{x: 5, y: {$lt: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {x : {$eq: 5}}, node: "
        "{ixscan: {filter: null, pattern: {x: 'hashed', y: 1},"
        "bounds: {x:[" +
        getHashedBound(5) + "], y: [[-Infinity,1,true,false]] }}}}}");
}

TEST_F(QueryPlannerHashedTest, PartialIndexCanAnswerPredicateOnFilteredFieldWithNonHashedPrefix) {
    auto filterObj = fromjson("{x: {$gt: 0}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"),
             filterExpr.get());

    runQuery(fromjson("{x: {$gte: 5}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {x: 1, y: 'hashed'},"
        "bounds: {x:[[5,Infinity,true,true]], y: [['MinKey','MaxKey',true,true]] }}}}}");

    runQuery(fromjson("{x: {$gte: 5}, y: 1}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {y : {$eq: 1}}, node: "
        "{ixscan: {filter: null, pattern: {x: 1, y: 'hashed'},"
        "bounds: {x: [[5,Infinity,true,true]], y:[" +
        getHashedBound(1) + "] }}}}}");
}

TEST_F(QueryPlannerHashedTest, PartialIndexDoesNotAnswerPredicatesExcludedByFilter) {
    auto filterObj = fromjson("{x: {$gt: 0}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);

    // Hashed prefix.
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1),
             filterExpr.get());

    runQuery(fromjson("{x: {$eq: -1}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{x: {$eq: 0}}"));
    assertHasOnlyCollscan();

    // Non-hashed prefix.
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"),
             filterExpr.get());

    runQuery(fromjson("{x: {$eq: -1}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{x: {$eq: 0}}"));
    assertHasOnlyCollscan();
}

//
// Hinting with hashed index tests.
//
TEST_F(QueryPlannerHashedTest, ChooseHashedIndexHint) {
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1));

    addIndex(BSON("x" << 1));
    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 3}, hint: {x: 'hashed', y: 1}}"));


    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 'hashed', y: 1}}}}}");
}

TEST_F(QueryPlannerHashedTest, ChooseHashedIndexHintWithOr) {
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1));

    addIndex(BSON("y" << 1));

    runQueryAsCommand(
        fromjson("{find: 'test', filter: {$or: [{x: 1}, {y: 1}]}, hint: {x: 'hashed', y: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {$or: [{x: 1}, {y: 1}]}, node: {ixscan: {pattern: {x: 'hashed', y: 1}, "
        "bounds: {x:[['MinKey','MaxKey',true,true]], y: [['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerHashedTest, HintWhenHashedIndexDoesNotExist) {
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1));

    runInvalidQueryHint(fromjson("{x: {$eq: 1}}"),
                        BSON("x"
                             << "hashed"));
}

TEST_F(QueryPlannerHashedTest, TypeOfWithHashedIndex) {
    // Hashed prefix.
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1));
    runQuery(fromjson("{x: {$type: 'number'}}"));
    assertHasOnlyCollscan();

    // Non-hashed prefix.
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"));

    runQuery(fromjson("{x: {$type: 'number'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {x: 1, y: 'hashed'}, "
        "bounds: {x:[[NaN,Infinity,true,true]], y: [['MinKey','MaxKey',true,true]]}}}}}");
}

//
// Collation tests.
//
TEST_F(QueryPlannerHashedTest,
       StringComparisonWithUnequalCollatorsAndHashedIndexResultsInCollscan) {
    CollatorInterfaceMock alwaysEqualCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    addIndex(BSON("a"
                  << "hashed"
                  << "b" << 1),
             &alwaysEqualCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 'foo'}}, collation: {locale: 'reverse'}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerHashedTest, StringComparisonWithEqualCollatorsAndHashedIndexUsesIndex) {
    CollatorInterfaceMock reverseStringCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(BSON("a"
                  << "hashed"
                  << "b" << 1),
             &reverseStringCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$eq: 'foo'}}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);

    // Verify that the bounds generated is based on a hash of reversed 'foo'.
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 'hashed', b: 1}, "
        "bounds: {a:[" +
        getHashedBound("oof") + "], b: [['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerHashedTest, NonStringComparisonWithUnequalCollators) {
    CollatorInterfaceMock alwaysEqualCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    addIndex(BSON("a"
                  << "hashed"
                  << "b" << 1),
             &alwaysEqualCollator);

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: 2}, collation: {locale: 'reverse'}}"));
    assertNumSolutions(1U);

    // Verify that we use an index scan even if the collators doesn't match.
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 'hashed', b: 1}, "
        "bounds: {a:[" +
        getHashedBound(2) + "], b: [['MinKey','MaxKey',true,true]]}}}}}");
}

//
// Tests to verify index usage for query with operators like $type, $regex, limit, skip etc.
//
TEST_F(QueryPlannerHashedTest, QueryWithPrefixRegex) {
    // Prefix regex cannot use index when prefix field is hashed.
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1 << "z" << -1));
    runQuery(fromjson("{x: /^foo/}"));
    assertHasOnlyCollscan();

    // Prefix regex can use index when prefix field is not hashed.
    addIndex(BSON("x" << 1 << "y"
                      << "hashed"));
    runQuery(fromjson("{x: /^foo/}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {x: 1, y: 'hashed'},"
        "bounds: {x: [['foo','fop',true,false], [/^foo/,/^foo/,true,true]], y: "
        "[['MinKey','MaxKey',true,true]] }}}}}");
}

TEST_F(QueryPlannerHashedTest, BasicSkip) {
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1));

    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 5}, skip: 8}"));
    assertNumSolutions(1U);

    // Verify that the plan has 'skip' stage and uses index.
    assertSolutionExists(
        "{skip: {n: 8, node: {fetch: {filter: { x : {$eq: 5}}, node: "
        "{ixscan: {filter: null, pattern: {x: 'hashed', y: 1},"
        "bounds: {x:[" +
        getHashedBound(5) + "], y: [['MinKey','MaxKey',true,true]] }}}}}}}");
}

TEST_F(QueryPlannerHashedTest, BasicLimit) {
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1));

    runQueryAsCommand(fromjson("{find: 'test', filter: {x: 5}, limit: 5}"));
    assertNumSolutions(1U);

    // Verify that the plan has 'limit' stage and uses index.
    assertSolutionExists(
        "{limit: {n: 5, node: {fetch: {filter: { x : {$eq: 5}}, node: "
        "{ixscan: {filter: null, pattern: {x: 'hashed', y: 1},"
        "bounds: {x:[" +
        getHashedBound(5) + "], y: [['MinKey','MaxKey',true,true]] }}}}}}}");
}

TEST_F(QueryPlannerHashedTest, MinMaxParameter) {
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1));

    runInvalidQueryHintMinMax(BSONObj(),
                              BSON("x"
                                   << "hashed"
                                   << "y" << 1),
                              fromjson("{x: 1}"),  // min.
                              BSONObj());
    runInvalidQueryHintMinMax(BSONObj(),
                              BSON("x"
                                   << "hashed"
                                   << "y" << 1),
                              BSONObj(),
                              fromjson("{x: 1}")  // max.
    );

    runQueryAsCommand(fromjson(
        "{find: 'test', filter: {x: 5}, hint: {x: 'hashed', y: 1}, min: {x: NumberLong(1), y: 2}, "
        "max: {x: NumberLong(2), y: 2}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {x: {$eq: 5}}, node: {ixscan: {filter: null, pattern: {x: 'hashed', y: "
        "1} }}}}");

    addIndex(BSON("x" << 1 << "y"
                      << "hashed"));
    runInvalidQueryHintMinMax(BSONObj(),
                              BSON("x" << 1 << "y"
                                       << "hashed"),
                              fromjson("{x: 1}"),  // min.
                              BSONObj());
    runInvalidQueryHintMinMax(BSONObj(),
                              BSON("x" << 1 << "y"
                                       << "hashed"),
                              BSONObj(),
                              fromjson("{x: 1}")  // max.
    );

    runQueryAsCommand(fromjson(
        "{find: 'test', filter: {x: 5}, hint: {x: 1, y: 'hashed'}, min: {x: NumberLong(1), y: 2}, "
        "max: {x: NumberLong(2), y: 2}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {x : {$eq: 5}}, node: {ixscan: {filter: null, pattern: {x: 1, y: "
        "'hashed'} }}}}");
}

TEST_F(QueryPlannerHashedTest, ExprEqCanUseIndex) {
    addIndex(BSON("x"
                  << "hashed"
                  << "y" << 1));
    runQuery(fromjson("{$expr: {$eq: ['$x', 1]}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{$expr: {$eq: ['$x', {$const: "
        "1}]}}]}, node: {ixscan: {pattern: {x: 'hashed', y: 1}, bounds: {x : [" +
        getHashedBound(1) + "], y: [['MinKey','MaxKey',true,true]] } }}}}");
}
}  // namespace mongo
