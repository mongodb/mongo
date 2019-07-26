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
    assert(isIxscan(db, explain.queryPlanner.winningPlan));
    explain = coll.explain("executionStats").find({x: {$gt: 1}, a: 1}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, explain.queryPlanner.winningPlan));
    explain = coll.explain("executionStats").find({x: 6, a: {$lte: 1}}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, explain.queryPlanner.winningPlan));

    // find() operations that should not use the index.
    explain = coll.explain("executionStats").find({x: 6, a: {$lt: 1.6}}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, explain.queryPlanner.winningPlan));

    explain = coll.explain("executionStats").find({x: 6}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, explain.queryPlanner.winningPlan));

    explain = coll.explain("executionStats").find({a: {$gte: 0}}).finish();
    assert.eq(2, explain.executionStats.nReturned);
    assert(isCollscan(db, explain.queryPlanner.winningPlan));
}

// Case where the partial filter expression is on a field in the index.
testPartialWildcardIndex({"$**": 1}, {partialFilterExpression: {a: {$lte: 1.5}}});

// Case where the partial filter expression is on a field not included in the index.
testPartialWildcardIndex({"x.$**": 1}, {partialFilterExpression: {a: {$lte: 1.5}}});
})();
