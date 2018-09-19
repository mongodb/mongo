/**
 * Test that $** indexes work when provided with a partial filter expression.
 *
 * TODO: SERVER-36198: Move this test back to jstests/core/
 */
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");  // For isIxScan, isCollscan.

    const coll = db.all_paths_partial_index;

    function testPartialAllPathsIndex(indexKeyPattern, indexOptions) {
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

    try {
        // Required in order to build $** indexes.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: true}));

        testPartialAllPathsIndex({"$**": 1}, {partialFilterExpression: {a: {$lte: 1.5}}});

        // Case where the partial filter expression is on a field not included in the index.
        testPartialAllPathsIndex({"x.$**": 1}, {partialFilterExpression: {a: {$lte: 1.5}}});
    } finally {
        // Disable $** indexes once the tests have either completed or failed.
        db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: false});

        // TODO: SERVER-36444 remove calls to drop() once wildcard index validation works.
        coll.drop();
    }
})();
