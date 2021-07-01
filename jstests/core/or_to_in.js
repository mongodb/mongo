// Tests the rewrite of single-path $or queries (where all disjuncts reference the same path)
// to an equivalent $in.
//
// This test is not prepared to handle explain output for sharded collections.
// @tags: [
//   assumes_unsharded_collection,
//   requires_fcv_49
// ]

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/libs/analyze_plan.js");

const coll = db.orToIn;
coll.drop();

function compareValues(v1, v2) {
    if (isNaN(v1) && isNaN(v2)) {
        return true;
    } else {
        return (v1 == v2);
    }
}

// Check that 'expectedQuery' and 'actualQuery' have the same plans, and produce the same result.
function assertEquivPlanAndResult(expectedQuery, actualQuery) {
    const expectedExplain = coll.find(expectedQuery).explain("queryPlanner");
    const actualExplain = coll.find(actualQuery).explain("queryPlanner");
    // The queries must be rewritten into the same form.
    assert.docEq(expectedExplain.parsedQuery, actualExplain.parsedQuery);
    // Make sure both queries have the same access plan.
    const expectedPlan = getWinningPlan(expectedExplain.queryPlanner);
    const actualPlan = getWinningPlan(actualExplain.queryPlanner);
    assert.docEq(expectedPlan, actualPlan);
    // The queries must produce the same result.
    const expectedRes = coll.find(expectedQuery).toArray();
    const actualRes = coll.find(actualQuery).toArray();
    assert(arrayEq(expectedRes, actualRes, false, compareValues),
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

assert.commandWorked(coll.insert([
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
]));

// Pairs of queries where the first one is expressed via OR (which is supposed to be
// rewritten as IN), and the second one is an equivalent query using IN.
const positiveTestQueries = [
    [{$or: [{f1: 5}, {f1: 3}, {f1: 7}]}, {f1: {$in: [7, 3, 5]}}],
    [{$or: [{f1: {$eq: 5}}, {f1: {$eq: 3}}, {f1: {$eq: 7}}]}, {f1: {$in: [7, 3, 5]}}],
    [{$or: [{f1: 42}, {f1: NaN}, {f1: 99}]}, {f1: {$in: [42, NaN, 99]}}],
    [{$or: [{f1: /^x/}, {f1: "ab"}]}, {f1: {$in: [/^x/, "ab"]}}],
    [{$or: [{f1: /^x/}, {f1: "^a"}]}, {f1: {$in: [/^x/, "^a"]}}],
    [{$or: [{f1: 42}, {f1: null}, {f1: 99}]}, {f1: {$in: [42, 99, null]}}],
    [{$or: [{f1: 1}, {f2: 9}, {f1: 99}]}, {$or: [{f2: 9}, {f1: {$in: [1, 99]}}]}],
    [{$or: [{f1: {$regex: /^x/}}, {f1: {$regex: /ab/}}]}, {f1: {$in: [/^x/, /ab/]}}],
    [
        {$and: [{$or: [{f1: 7}, {f1: 3}, {f1: 5}]}, {$or: [{f1: 1}, {f1: 2}, {f1: 3}]}]},
        {$and: [{f1: {$in: [7, 3, 5]}}, {f1: {$in: [1, 2, 3]}}]}
    ],
    [
        {$or: [{$or: [{f1: 7}, {f1: 3}, {f1: 5}]}, {$or: [{f1: 1}, {f1: 2}, {f1: 3}]}]},
        {$or: [{f1: {$in: [7, 3, 5]}}, {f1: {$in: [1, 2, 3]}}]}
    ],
    [
        {$or: [{$and: [{f1: 7}, {f2: 7}, {f1: 5}]}, {$or: [{f1: 1}, {f1: 2}, {f1: 3}]}]},
        {$or: [{$and: [{f1: 7}, {f2: 7}, {f1: 5}]}, {f1: {$in: [1, 2, 3]}}]},
    ],
    [{$or: [{f2: [32, 52]}, {f2: [42, [13, 11]]}]}, {f2: {$in: [[32, 52], [42, [13, 11]]]}}],
    [{$or: [{f2: 52}, {f2: 13}]}, {f2: {$in: [52, 13]}}],
    [{$or: [{f2: [11]}, {f2: [23]}]}, {f2: {$in: [[11], [23]]}}],
    {$or: [{f1: 42}, {f1: null}]},
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

function testOrToIn(queries) {
    for (const queryPair of queries) {
        assertEquivPlanAndResult(queryPair[0], queryPair[1]);
    }
}

testOrToIn(positiveTestQueries);  // test without indexes

assert.commandWorked(coll.createIndex({f1: 1}));

testOrToIn(positiveTestQueries);  // single index

assert.commandWorked(coll.createIndex({f2: 1}));
assert.commandWorked(coll.createIndex({f1: 1, f2: 1}));

testOrToIn(positiveTestQueries);  // three indexes, requires multiplanning
}());
