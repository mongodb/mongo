// Read ops tests for partial indexes.

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

(function() {
    "use strict";
    var explain;
    var coll = db.index_partial_read_ops;
    coll.drop();

    assert.commandWorked(coll.ensureIndex({x: 1}, {partialFilterExpression: {a: {$lte: 1.5}}}));
    assert.writeOK(coll.insert({x: 5, a: 2}));  // Not in index.
    assert.writeOK(coll.insert({x: 6, a: 1}));  // In index.

    //
    // Verify basic functionality with find().
    //

    // find() operations that should use index.
    explain = coll.explain('executionStats').find({x: 6, a: 1}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(explain.queryPlanner.winningPlan));
    explain = coll.explain('executionStats').find({x: {$gt: 1}, a: 1}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(explain.queryPlanner.winningPlan));
    explain = coll.explain('executionStats').find({x: 6, a: {$lte: 1}}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(explain.queryPlanner.winningPlan));

    // find() operations that should not use index.
    explain = coll.explain('executionStats').find({x: 6, a: {$lt: 1.6}}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(explain.queryPlanner.winningPlan));
    explain = coll.explain('executionStats').find({x: 6}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(explain.queryPlanner.winningPlan));

    //
    // Verify basic functionality with the count command.
    //

    // Count operation that should use index.
    explain = coll.explain('executionStats').count({x: {$gt: 1}, a: 1});
    assert(isIxscan(explain.queryPlanner.winningPlan));

    // Count operation that should not use index.
    explain = coll.explain('executionStats').count({x: {$gt: 1}, a: 2});
    assert(isCollscan(explain.queryPlanner.winningPlan));

    //
    // Verify basic functionality with the aggregate command.
    //

    // Aggregate operation that should use index.
    explain = coll.aggregate([{$match: {x: {$gt: 1}, a: 1}}], {explain: true}).stages[0].$cursor;
    assert(isIxscan(explain.queryPlanner.winningPlan));

    // Aggregate operation that should not use index.
    explain = coll.aggregate([{$match: {x: {$gt: 1}, a: 2}}], {explain: true}).stages[0].$cursor;
    assert(isCollscan(explain.queryPlanner.winningPlan));

    //
    // Verify basic functionality with the findAndModify command.
    //

    // findAndModify operation that should use index.
    explain = coll.explain('executionStats')
                  .findAndModify({query: {x: {$gt: 1}, a: 1}, update: {$inc: {x: 1}}});
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(explain.queryPlanner.winningPlan));

    // findAndModify operation that should not use index.
    explain = coll.explain('executionStats')
                  .findAndModify({query: {x: {$gt: 1}, a: 2}, update: {$inc: {x: 1}}});
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(explain.queryPlanner.winningPlan));
})();
