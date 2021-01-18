// Tests for whether the query solution correctly used an AND_HASH for index intersection.
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/libs/analyze_plan.js");  // For planHasStage helper to analyze explain() output.

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

// Enable and force AND_HASH query solution to be used to evaluate index intersections.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: true}));
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryPlannerEnableHashIntersection: true}));

const coll = db.and_hash;
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.createIndex({c: -1}));
assert.commandWorked(coll.createIndex({e: 1}));
assert.commandWorked(coll.createIndex({f: 1}));

assert.commandWorked(coll.insertMany([
    {_id: 0, a: 1, b: "A", c: 22, d: "field"},
    {_id: 1, a: 1, b: "B", c: 22, d: "field"},
    {_id: 2, a: 1, b: "C", c: 22, d: "field"},
    {_id: 3, a: 2, b: 2, c: null},
    {_id: 4, a: 2, b: "D", c: 22},
    {_id: 5, a: 2, b: "E", c: 22, d: 99},
    {_id: 6, a: 3, b: "b", c: 23, d: "field"},
    {_id: 7, a: 3, b: "abc", c: 22, d: 23},
    {_id: 8, a: 3, b: "A", c: 22, d: "field"},
    {_id: 9, a: 4, b: "a", c: {x: 1, y: 2}, d: "field"},
    {_id: 10, a: 5, b: "ABC", d: 22, c: "field"},
    {_id: 11, a: 6, b: "ABC", d: 22, c: "field"},
    {_id: 12, a: 4, b: "a", c: {x: 1, y: 2}, d: "field", e: [1, 2, 3]},
    {_id: 13, a: 5, b: "ABC", d: 22, c: "field", e: [4, 5, 6]},
    {_id: 14, a: 6, b: "ABC", d: 22, c: "field", e: [7, 8, 9]},
    {_id: 15, a: 1, e: [7, 8, 9], f: [-1, -2, -3]},
]));

// Helper to check the result returned by the query and to check whether the
// query solution correctly did or did not use an AND_HASH for index
// intersection.
function assertAndHashUsed({query, expectedResult, shouldUseAndHash} = {}) {
    const queryResult = coll.find(query);
    const expl = queryResult.explain();

    assertArrayEq({actual: queryResult.toArray(), expected: expectedResult});
    assert.eq(shouldUseAndHash, planHasStage(db, getWinningPlan(expl.queryPlanner), "AND_HASH"));
}

// Test basic index intersection where we expect AND_HASH to be used.
assertAndHashUsed({
    query: {a: {$gt: 1}, c: null},
    expectedResult: [{_id: 3, a: 2, b: 2, c: null}],
    shouldUseAndHash: true
});
assertAndHashUsed({
    query: {a: {$gt: 3}, c: {x: 1, y: 2}},
    expectedResult: [
        {_id: 9, a: 4, b: "a", c: {x: 1, y: 2}, d: "field"},
        {_id: 12, a: 4, b: "a", c: {x: 1, y: 2}, d: "field", e: [1, 2, 3]}
    ],
    shouldUseAndHash: true
});
assertAndHashUsed({
    query: {a: {$lt: 5}, b: {$in: ["A", "abc"]}},
    expectedResult: [
        {_id: 0, a: 1, b: "A", c: 22, d: "field"},
        {_id: 7, a: 3, b: "abc", c: 22, d: 23},
        {_id: 8, a: 3, b: "A", c: 22, d: "field"},
    ],
    shouldUseAndHash: true
});
assertAndHashUsed({
    query: {a: {$gt: 1}, e: {$elemMatch: {$lt: 7}}},
    expectedResult: [
        {_id: 12, a: 4, b: "a", c: {x: 1, y: 2}, d: "field", e: [1, 2, 3]},
        {_id: 13, a: 5, b: "ABC", d: 22, c: "field", e: [4, 5, 6]},
    ],
    shouldUseAndHash: true
});
assertAndHashUsed({query: {a: {$gt: 5}, c: {$lt: 3}}, expectedResult: [], shouldUseAndHash: true});
assertAndHashUsed({query: {a: {$gt: 5}, c: null}, expectedResult: [], shouldUseAndHash: true});
assertAndHashUsed({query: {a: {$gt: 1}, c: {$lt: 3}}, expectedResult: [], shouldUseAndHash: true});

// Test queries that should not use AND_HASH.
assertAndHashUsed({
    query: {a: 6},
    expectedResult: [
        {_id: 11, a: 6, b: "ABC", d: 22, c: "field"},
        {_id: 14, a: 6, b: "ABC", d: 22, c: "field", e: [7, 8, 9]}
    ],
    shouldUseAndHash: false
});
assertAndHashUsed({query: {fieldDoesNotExist: 1}, expectedResult: [], shouldUseAndHash: false});
assertAndHashUsed({
    query: {$or: [{a: 6}, {fieldDoesNotExist: 1}]},
    expectedResult: [
        {_id: 11, a: 6, b: "ABC", d: 22, c: "field"},
        {_id: 14, a: 6, b: "ABC", d: 22, c: "field", e: [7, 8, 9]}
    ],
    shouldUseAndHash: false
});
assertAndHashUsed({query: {$or: [{a: 7}, {a: 8}]}, expectedResult: [], shouldUseAndHash: false});

// Test intersection with a compound index.
assert.commandWorked(coll.createIndex({c: 1, d: -1}));
assertAndHashUsed({
    query: {a: {$gt: 1}, c: {$gt: 0}, d: {$gt: 90}},
    expectedResult: [{_id: 5, a: 2, b: "E", c: 22, d: 99}],
    shouldUseAndHash: true
});
// The query on only 'd' field should not be able to use the index and thus the intersection
// since it is not using a prefix of the compound index {c:1, d:-1}.
assertAndHashUsed({
    query: {a: {$gt: 1}, d: {$gt: 90}},
    expectedResult: [{_id: 5, a: 2, b: "E", c: 22, d: 99}],
    shouldUseAndHash: false
});

// Test that deduplication is correctly performed on top of the index scans that comprise the
// hash join -- predicate matching multiple elements of array should not return the same
// document more than once.
assertAndHashUsed({
    query: {e: {$gt: 0}, a: 6},
    expectedResult: [{_id: 14, a: 6, b: "ABC", d: 22, c: "field", e: [7, 8, 9]}],
    shouldUseAndHash: true
});
assertAndHashUsed({
    query: {e: {$gt: 0}, f: {$lt: 0}},
    expectedResult: [{_id: 15, a: 1, e: [7, 8, 9], f: [-1, -2, -3]}],
    shouldUseAndHash: true
});

MongoRunner.stopMongod(conn);
})();
