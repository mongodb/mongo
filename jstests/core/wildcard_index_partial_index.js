/**
 * Test that $** indexes work when provided with a partial filter expression.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For isIxScan, isCollscan.

const coll = db.wildcard_partial_index;

function testPartialWildcardIndex(indexKeyPattern, indexOptions) {
    coll.drop();

    assert.commandWorked(coll.createIndex(indexKeyPattern, indexOptions));
    assert.commandWorked(coll.insert({x: 5, a: 2}));  // Not in index.
    assert.commandWorked(coll.insert({x: 6, a: 1}));  // In index.

    // find() operations that should use the index.
    let explain = coll.explain("executionStats").find({x: 6, a: 1}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
    explain = coll.explain("executionStats").find({x: {$gt: 1}, a: 1}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
    explain = coll.explain("executionStats").find({x: 6, a: {$lte: 1}}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));

    // find() operations that should not use the index.
    explain = coll.explain("executionStats").find({x: 6, a: {$lt: 1.6}}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));

    explain = coll.explain("executionStats").find({x: 6}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));

    explain = coll.explain("executionStats").find({a: {$gte: 0}}).finish();
    assert.eq(2, explain.executionStats.nReturned);
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));
}

// Case where the partial filter expression is on a field in the index.
testPartialWildcardIndex({"$**": 1}, {partialFilterExpression: {a: {$lte: 1.5}}});

// Case where the partial filter expression is on a field not included in the index.
testPartialWildcardIndex({"x.$**": 1}, {partialFilterExpression: {a: {$lte: 1.5}}});

// This part of this test is designed to reproduce SERVER-48614. Historically, the correctness of
// the following queries was impacted by a bug in the plan caching system.
coll.drop();
assert.commandWorked(coll.createIndex({"$**": 1}, {partialFilterExpression: {x: 1}}));
assert.commandWorked(coll.insert({x: 1}));

// Produce an active plan cache entry for a query that can use the index.
for (let i = 0; i < 2; ++i) {
    assert.eq(0, coll.find({x: 1, y: 1}).itcount());
}
// Run a query with a similar shape, but which is not eligible to use the cached plan. This query
// should match the document in the collection (but would fail to match if it incorrectly indexed
// the $eq:null predicate using the wildcard index).
assert.eq(1, coll.find({x: 1, y: null}).itcount());
})();
