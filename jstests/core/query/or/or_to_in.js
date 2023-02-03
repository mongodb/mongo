// Tests the rewrite of single-path $or queries (where all disjuncts reference the same path)
// to an equivalent $in.
//
// This test is not prepared to handle explain output for sharded collections.
// @tags: [
//   assumes_unsharded_collection,
// ]

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");  // For checkBothEnginesAreRunOnCluster.

var coll = db.orToIn;
coll.drop();

function compareValues(v1, v2) {
    if (isNaN(v1) && isNaN(v2)) {
        return true;
    } else {
        return (v1 == v2);
    }
}

// Check that 'expectedQuery' and 'actualQuery' have the same plans, and produce the same result.
function assertEquivPlanAndResult(expectedQuery, actualQuery, supportWithCollation) {
    const expectedExplain = coll.find(expectedQuery).explain("queryPlanner");
    const actualExplain = coll.find(actualQuery).explain("queryPlanner");

    // The queries must be rewritten into the same form.
    assert.docEq(expectedExplain.queryPlanner.parsedQuery, actualExplain.queryPlanner.parsedQuery);

    // We are always running these queries to ensure a server crash is not triggered.
    // TODO SERVER-72450: Add appropriate assertions for the output.
    const expectedExplainCollation =
        coll.find(expectedQuery).sort({f1: 1}).collation({locale: 'en_US'}).explain("queryPlanner");
    const actualExplainCollation =
        coll.find(actualQuery).sort({f1: 1}).collation({locale: 'en_US'}).explain("queryPlanner");

    if (supportWithCollation) {
        // Check if the test queries produce the same plans with collations.
        assert.docEq(expectedExplainCollation.queryPlanner.parsedQuery,
                     actualExplainCollation.queryPlanner.parsedQuery);
    }

    // Make sure both queries have the same access plan.
    const expectedPlan = getWinningPlan(expectedExplain.queryPlanner);
    const actualPlan = getWinningPlan(actualExplain.queryPlanner);
    if (!checkBothEnginesAreRunOnCluster(db)) {
        assert.docEq(expectedPlan, actualPlan);
    }

    // The queries must produce the same result.
    const expectedRes = coll.find(expectedQuery).toArray();
    const actualRes = coll.find(actualQuery).toArray();
    assert(arrayEq(expectedRes, actualRes, false, compareValues),
           `expected=${expectedRes}, actual=${actualRes}`);

    // also with collation
    const expectedResCollation =
        coll.find(expectedQuery).sort({f1: 1}).collation({locale: 'en_US'}).toArray();
    const actualResCollation =
        coll.find(actualQuery).sort({f1: 1}).collation({locale: 'en_US'}).toArray();
    assert(arrayEq(expectedResCollation, actualResCollation, false, compareValues),
           `expected=${expectedRes}, actual=${actualRes}`);
}

// Make sure that certain $or expressions are not rewritten to $in.
// This is the case when $eq is not equivalent to the implied equality
// used by $in.
function assertOrNotRewrittenToIn(query) {
    const parsedQuery = coll.find(query).explain("queryPlanner").queryPlanner.parsedQuery;
    const topOp = Object.keys(parsedQuery);
    assert(arrayEq(topOp, ["$or"]));
    // None of the children should be $in.
    for (const child of parsedQuery["$or"]) {
        const childOp = Object.keys(child);
        const childOfChild = Object.keys(child[childOp[0]]);
        assert(!arrayEq(childOfChild, ["$in"]), `expected=["$in"], actual=${childOfChild}`);
    }
}

const data = [
    {_id: 0, f1: 3, f2: 7},
    {_id: 1, f1: 1, f2: [32, 42, 52, [11]]},
    {_id: 2, f1: 2, f2: 9},
    {_id: 3, f1: 5, f2: 42},
    {_id: 4, f1: 1, f2: [23, 42, 13]},
    {_id: 5, f1: "xz", f2: 13},
    {_id: 6, f1: "ab", f2: 15},
    {_id: 7, f1: null, f2: null},
    {_id: 8, f1: undefined, f2: undefined},
    {_id: 9, f1: NaN, f2: NaN},
    {_id: 10, f1: 1, f2: [32, 52]},
    {_id: 11, f1: 1, f2: [42, [13, 11]]}
];

assert.commandWorked(coll.insert(data));

// Pairs of queries where the first one is expressed via OR (which is supposed to be
// rewritten as IN), and the second one is an equivalent query using IN.
//
// The third element of the array is optional, if present, implies that the rewrite is not
// supported when there is a collation involved.
//
// TODO SERVER-72450: Remove or update this logic related to collation, and enforce stronger
// assertions.
const positiveTestQueries = [
    {actualQuery: {$or: [{f1: 5}, {f1: 3}, {f1: 7}]}, expectedQuery: {f1: {$in: [7, 3, 5]}}},
    {
        actualQuery: {$or: [{f1: {$eq: 5}}, {f1: {$eq: 3}}, {f1: {$eq: 7}}]},
        expectedQuery: {f1: {$in: [7, 3, 5]}}
    },
    {
        actualQuery: {$or: [{f1: 42}, {f1: NaN}, {f1: 99}]},
        expectedQuery: {f1: {$in: [42, NaN, 99]}}
    },
    {
        actualQuery: {$or: [{f1: /^x/}, {f1: "ab"}]},
        expectedQuery: {f1: {$in: [/^x/, "ab"]}},
        cannotRewriteWithCollation: true
    },
    {
        actualQuery: {$or: [{f1: /^x/}, {f1: "^a"}]},
        expectedQuery: {f1: {$in: [/^x/, "^a"]}},
        cannotRewriteWithCollation: true
    },
    {
        actualQuery: {$or: [{f1: 42}, {f1: null}, {f1: 99}]},
        expectedQuery: {f1: {$in: [42, 99, null]}}
    },
    {
        actualQuery: {$or: [{f1: 1}, {f2: 9}, {f1: 99}]},
        expectedQuery: {$or: [{f2: 9}, {f1: {$in: [1, 99]}}]}
    },
    {
        actualQuery: {$or: [{f1: {$regex: /^x/}}, {f1: {$regex: /ab/}}]},
        expectedQuery: {f1: {$in: [/^x/, /ab/]}}
    },
    {
        actualQuery:
            {$and: [{$or: [{f1: 7}, {f1: 3}, {f1: 5}]}, {$or: [{f1: 1}, {f1: 2}, {f1: 3}]}]},
        expectedQuery: {$and: [{f1: {$in: [7, 3, 5]}}, {f1: {$in: [1, 2, 3]}}]}
    },
    {
        actualQuery:
            {$or: [{$or: [{f1: 7}, {f1: 3}, {f1: 5}]}, {$or: [{f1: 1}, {f1: 2}, {f1: 3}]}]},
        expectedQuery: {$or: [{f1: {$in: [7, 3, 5]}}, {f1: {$in: [1, 2, 3]}}]}
    },
    {
        actualQuery:
            {$or: [{$and: [{f1: 7}, {f2: 7}, {f1: 5}]}, {$or: [{f1: 1}, {f1: 2}, {f1: 3}]}]},
        expectedQuery: {$or: [{$and: [{f1: 7}, {f2: 7}, {f1: 5}]}, {f1: {$in: [1, 2, 3]}}]},
    },
    {
        actualQuery: {$or: [{f2: [32, 52]}, {f2: [42, [13, 11]]}]},
        expectedQuery: {f2: {$in: [[32, 52], [42, [13, 11]]]}}
    },
    {actualQuery: {$or: [{f2: 52}, {f2: 13}]}, expectedQuery: {f2: {$in: [52, 13]}}},
    {actualQuery: {$or: [{f2: [11]}, {f2: [23]}]}, expectedQuery: {f2: {$in: [[11], [23]]}}},
    {actualQuery: {$or: [{f1: 42}, {f1: null}]}, expectedQuery: {f1: {$in: [42, null]}}},
    {
        actualQuery: {$or: [{f1: "a"}, {f1: "b"}, {f1: /c/}]},
        expectedQuery: {f1: {$in: ["a", "b", /c/]}},
        cannotRewriteWithCollation: true
    },
];

// These $or queries should not be rewritten into $in because of different semantics.
const negativeTestQueries = [
    {$or: [{f2: 9}, {f1: 1}, {f1: 99}]},
    {$or: [{f1: {$eq: /^x/}}, {f1: {$eq: /ab/}}]},
    {$or: [{f1: {$lt: 2}}, {f1: {$gt: 3}}]},
];

for (const query of negativeTestQueries) {
    assertOrNotRewrittenToIn(query);
}

function testOrToIn(queries, usesCollation) {
    for (const queryPair of queries) {
        if (usesCollation && queryPair.cannotRewriteWithCollation) {
            continue;
        }
        assertEquivPlanAndResult(
            queryPair.actualQuery, queryPair.expectedQuery, !queryPair.cannotRewriteWithCollation);
    }
}

testOrToIn(positiveTestQueries, false /* usesCollation */);  // test without indexes

assert.commandWorked(coll.createIndex({f1: 1}));

testOrToIn(positiveTestQueries, false /* usesCollation */);  // single index

assert.commandWorked(coll.createIndex({f2: 1}));
assert.commandWorked(coll.createIndex({f1: 1, f2: 1}));

testOrToIn(positiveTestQueries,
           false /* usesCollation */);  // three indexes, requires multiplanning

// Test with a collection that has a collation, and that collation is the same as the query
// collation
coll.drop();
assert.commandWorked(db.createCollection("orToIn", {collation: {locale: 'en_US'}}));
coll = db.orToIn;
assert.commandWorked(coll.insert(data));
testOrToIn(positiveTestQueries, true /* usesCollation */);
// Test with a collection that has a collation, and that collation is different from the query
// collation
coll.drop();
assert.commandWorked(db.createCollection("orToIn", {collation: {locale: 'de'}}));
coll = db.orToIn;
assert.commandWorked(coll.insert(data));
testOrToIn(positiveTestQueries, true /* usesCollation */);
}());
