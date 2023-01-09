// Cannot implicitly shard accessed collections because the explain output from a mongod when run
// against a sharded collection is wrapped in a "shards" object with keys for each shard.
// @tags: [
//   assumes_unsharded_collection,
//   does_not_support_stepdowns,
//   requires_fcv_51,
//   # TODO SERVER-30466
//   does_not_support_causal_consistency,
// ]

// Read ops tests for partial indexes.

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");
load("jstests/libs/feature_flag_util.js");

(function() {
"use strict";
let explain;
const coll = db.index_partial_read_ops;

(function testBasicPartialFilterExpression() {
    coll.drop();

    assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {a: {$lte: 1.5}}}));
    assert.commandWorked(coll.insert({x: 5, a: 2}));  // Not in index.
    assert.commandWorked(coll.insert({x: 6, a: 1}));  // In index.

    //
    // Verify basic functionality with find().
    //

    // find() operations that should use index.
    explain = coll.explain('executionStats').find({x: 6, a: 1}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
    explain = coll.explain('executionStats').find({x: {$gt: 1}, a: 1}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
    explain = coll.explain('executionStats').find({x: 6, a: {$lte: 1}}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));

    // find() operations that should not use index.
    explain = coll.explain('executionStats').find({x: 6, a: {$lt: 1.6}}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));
    explain = coll.explain('executionStats').find({x: 6}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));

    //
    // Verify basic functionality with the count command.
    //

    // Count operation that should use index.
    explain = coll.explain('executionStats').count({x: {$gt: 1}, a: 1});
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));

    // Count operation that should not use index.
    explain = coll.explain('executionStats').count({x: {$gt: 1}, a: 2});
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));

    //
    // Verify basic functionality with the aggregate command.
    //

    // Aggregate operation that should use index.
    explain = coll.aggregate([{$match: {x: {$gt: 1}, a: 1}}], {explain: true});
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));

    // Aggregate operation that should not use index.
    explain = coll.aggregate([{$match: {x: {$gt: 1}, a: 2}}], {explain: true});
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));

    //
    // Verify basic functionality with the findAndModify command.
    //

    // findAndModify operation that should use index.
    explain = coll.explain('executionStats')
                  .findAndModify({query: {x: {$gt: 1}, a: 1}, update: {$inc: {x: 1}}});
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));

    // findAndModify operation that should not use index.
    explain = coll.explain('executionStats')
                  .findAndModify({query: {x: {$gt: 1}, a: 2}, update: {$inc: {x: 1}}});
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));

    //
    // Verify functionality with multiple overlapping partial indexes on the same key pattern.
    //

    // Remove existing indexes and documents.
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.remove({}));

    assert.commandWorked(
        coll.createIndex({a: 1}, {name: "index1", partialFilterExpression: {a: {$gte: 0}}}));
    assert.commandWorked(
        coll.createIndex({a: 1}, {name: "index2", partialFilterExpression: {a: {$gte: 10}}}));
    assert.commandWorked(
        coll.createIndex({a: 1}, {name: "index3", partialFilterExpression: {a: {$gte: 100}}}));

    assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}]));
    assert.commandWorked(coll.insert([{a: 11}, {a: 12}, {a: 13}]));
    assert.commandWorked(coll.insert([{a: 101}, {a: 102}, {a: 103}]));

    // Function which verifies that the given query is indexed, that it produces the same output as
    // a COLLSCAN and the given 'expectedResults' array, and that 'numAlternativePlans' were
    // generated.
    function assertIndexedQueryAndResults(query, numAlternativePlans, expectedResults) {
        const explainOut = coll.explain().find(query).finish();
        const results = coll.find(query).toArray();
        assert(isIxscan(db, explainOut), tojson(explainOut));
        assert.eq(getRejectedPlans(explainOut).length, numAlternativePlans, tojson(explainOut));
        assert.sameMembers(results, coll.find(query).hint({$natural: 1}).toArray());
        assert.sameMembers(results.map(doc => (delete doc._id && doc)), expectedResults);
    }

    // Queries which fall within the covered ranges generate plans for all applicable partial
    // indexes.
    assertIndexedQueryAndResults({a: {$gt: 0, $lt: 10}}, 0, [{a: 1}, {a: 2}, {a: 3}]);
    assertIndexedQueryAndResults({a: {$gt: 10, $lt: 100}}, 1, [{a: 11}, {a: 12}, {a: 13}]);
    assertIndexedQueryAndResults({a: {$gt: 100, $lt: 1000}}, 2, [{a: 101}, {a: 102}, {a: 103}]);
    assertIndexedQueryAndResults(
        {a: {$gt: 0}},
        0,
        [{a: 1}, {a: 2}, {a: 3}, {a: 11}, {a: 12}, {a: 13}, {a: 101}, {a: 102}, {a: 103}]);

    // Queries which fall outside the range of any partial indexes produce a COLLSCAN.
    assert(isCollscan(db, coll.explain().find({a: {$lt: 0}}).finish()));
})();

if (!FeatureFlagUtil.isEnabled(db, "TimeseriesMetricIndexes")) {
    jsTest.log(
        "Skipping partialFilterExpression testing for $in, $or and non-top level $and as timeseriesMetricIndexesEnabled is false");
    return;
}

(function testFilterWithInExpression() {
    assert(coll.drop());

    assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {a: {$in: [3, 5]}}}));
    assert.commandWorked(coll.insert({x: 1, a: 4}));  // Not in index.
    assert.commandWorked(coll.insert({x: 2, a: 3}));  // In index.
    assert.commandWorked(coll.insert({x: 3, a: 5}));  // In index.

    // find() operations that should use index.
    explain = coll.explain('executionStats').find({x: 2, a: 3}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
    explain = coll.explain('executionStats').find({x: {$gt: 1}, a: 5}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));

    // find() operations that should not use index.
    explain = coll.explain('executionStats').find({x: 3, a: {$lt: 6}}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));
    explain = coll.explain('executionStats').find({x: 2}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));
})();

(function testFilterWithMultiLevelAndOrExpressions() {
    assert(coll.drop());

    assert.commandWorked(coll.createIndex(
        {x: 1}, {partialFilterExpression: {$or: [{a: 3}, {$and: [{a: 5}, {b: 5}]}]}}));
    assert.commandWorked(coll.insert({x: 1, a: 1}));        // Not in index.
    assert.commandWorked(coll.insert({x: 2, a: 5}));        // Not in index.
    assert.commandWorked(coll.insert({x: 3, a: 5, b: 1}));  // Not in index.
    assert.commandWorked(coll.insert({x: 4, a: 3}));        // In index.
    assert.commandWorked(coll.insert({x: 5, a: 5, b: 5}));  // In index.

    // find() operations that should use index.
    explain = coll.explain('executionStats').find({x: 4, a: 3}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
    explain = coll.explain('executionStats').find({x: 5, a: 5, b: 5}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)), explain.queryPlanner);

    // find() operations that should not use index.
    explain = coll.explain('executionStats').find({x: 1, a: 1}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));
    explain = coll.explain('executionStats').find({x: 2, a: 5}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));
    explain = coll.explain('executionStats').find({x: 3, a: 5, b: 1}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, getWinningPlan(explain.queryPlanner)));
})();
})();
