//
// Tests that queries in sharded collections will be properly optimized
//
// @tags: [requires_fcv_48]

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

function assertShardFilter(explain) {
    const filterStage = getPlanStage(explain.queryPlanner.winningPlan, "SHARDING_FILTER");
    assert.eq(filterStage.stage, "SHARDING_FILTER");
    const scanStage = filterStage.inputStage;
    assert.contains(scanStage.stage, ["IXSCAN", "FETCH"]);
}

function assertNoShardFilter(explain) {
    const filterStage = getPlanStage(explain.queryPlanner.winningPlan, "SHARDING_FILTER");
    assert.eq(filterStage, null, explain);
}

function assertCountScan(explain) {
    const countStage = getPlanStage(explain.queryPlanner.winningPlan, "COUNT_SCAN");
    assert.eq(countStage.stage, "COUNT_SCAN");
}

const st = new ShardingTest({shards: 1});
const coll = st.s0.getCollection("foo.bar");

function createCollection(coll, shardKey) {
    coll.drop();
    assert.commandWorked(st.s0.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

    assert.commandWorked(coll.insert({_id: true, a: true, b: true, c: true, d: true}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.commandWorked(coll.createIndex({b: 1, a: 1}));
}

assert.commandWorked(st.s0.adminCommand({enableSharding: coll.getDB().getName()}));

jsTest.log('Tests with single shard key');
createCollection(coll, {a: 1});

// We're requesting a specific shard key, therefore we should optimize away SHARDING_FILTER
// and use a cheaper COUNT_SCAN.
let explain = assert.commandWorked(coll.explain('executionStats').count({a: true}));
assertCountScan(explain);
// Check this works with a subset of records as well.
explain = assert.commandWorked(coll.explain('executionStats').count({a: true, b: true}));
assertCountScan(explain);

// Test that a find() query which specifies the entire shard key does not need a shard filter.
explain = assert.commandWorked(coll.find({a: true}).explain());
assertNoShardFilter(explain);
explain = assert.commandWorked(coll.find({a: true, b: true}).explain());
assertNoShardFilter(explain);

// We're not checking shard key for equality, therefore need a sharding filter.
explain = assert.commandWorked(coll.explain('executionStats').count({a: {$in: [true, false]}}));
assertShardFilter(explain);

// We're requesting a disjoint key from shardkey, therefore need a sharding filter.
explain = assert.commandWorked(coll.explain('executionStats').count({b: true}));
assertShardFilter(explain);

jsTest.log('Tests with compound shard key');
createCollection(coll, {a: 1, b: 1});

explain = assert.commandWorked(coll.explain('executionStats').count({a: true}));
assertShardFilter(explain);
explain = assert.commandWorked(coll.explain('executionStats').count({a: true, b: true}));
assertCountScan(explain);
explain =
    assert.commandWorked(coll.explain('executionStats').count({a: true, b: {$in: [true, false]}}));
assertShardFilter(explain);

explain = assert.commandWorked(coll.find({a: true}).explain());
assertShardFilter(explain);
explain = assert.commandWorked(coll.find({a: true, b: true}).explain());
assertNoShardFilter(explain);
explain = assert.commandWorked(coll.find({a: true, b: {$in: [true, false]}}).explain());
assertShardFilter(explain);

jsTest.log('Tests with hashed shard key');
createCollection(coll, {a: 'hashed'});

explain = assert.commandWorked(coll.explain('executionStats').count({a: true}));
assertCountScan(explain);
explain = assert.commandWorked(coll.explain('executionStats').count({a: true, b: true}));
assertCountScan(explain);
explain = assert.commandWorked(coll.explain('executionStats').count({a: {$in: [true, false]}}));
assertShardFilter(explain);
explain = assert.commandWorked(coll.explain('executionStats').count({b: true}));
assertShardFilter(explain);

explain = assert.commandWorked(coll.find({a: true}).explain());
assertNoShardFilter(explain);
explain = assert.commandWorked(coll.find({a: true, b: true}).explain());
assertNoShardFilter(explain);
explain = assert.commandWorked(coll.find({a: {$in: [true, false]}}).explain());
assertShardFilter(explain);
explain = assert.commandWorked(coll.find({b: true}).explain());
assertShardFilter(explain);

st.stop();
})();
