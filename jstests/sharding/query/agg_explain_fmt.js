/* This test ensures that explain of an aggregate through mongos has the intended format.

 * @tags: [
 * # $mergeCursors was added to explain output in 5.3.
 * requires_fcv_53
 * ]
 */

(function() {
"use strict";

load('jstests/libs/analyze_plan.js');  // For planHasStage.

const st = new ShardingTest({shards: 2});
const mongosDB = st.s.getDB("test");
const coll = mongosDB.agg_explain_fmt;
// Insert documents with {_id: -5} to {_id: 4}.
assert.commandWorked(coll.insert(Array.from({length: 10}, (_, i) => ({_id: i - 5}))));

// Test that with an unsharded collection we don't get any kind of 'splitPipeline', just the
// normal explain with 'stages'.
let explain = coll.explain().aggregate([{$project: {a: 1}}]);
assert(!explain.hasOwnProperty("splitPipeline"), explain);
assert(planHasStage(mongosDB, explain, "PROJECTION_SIMPLE"), explain);

// Now shard the collection by _id and move a chunk to each shard.
st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});

// Test that we now have a split pipeline with information about what pipeline ran on each
// shard.
explain = coll.explain().aggregate([{$project: {a: 1}}]);
assert(explain.hasOwnProperty("splitPipeline"), explain);
assert(explain.splitPipeline.hasOwnProperty("shardsPart"), explain.splitPipeline);
assert(explain.splitPipeline.hasOwnProperty("mergerPart"), explain.splitPipeline);
assert(explain.hasOwnProperty("shards"), explain);
for (let shardId in explain.shards) {
    const shardExplain = explain.shards[shardId];
    assert(shardExplain.hasOwnProperty("host"), shardExplain);
    assert(shardExplain.hasOwnProperty("stages") || shardExplain.hasOwnProperty("queryPlanner"),
           shardExplain);
}

// Test that the $mergeCursors stage is present in the mergerPart of the pipeline.
explain = coll.explain().aggregate(
    [{$match: {mykey: {'$in': [0, 1, 3, 7]}}}, {'$sort': {favColor: 1}}, {$limit: 5}]);

assert(explain.hasOwnProperty("splitPipeline"), explain);
assert(explain.splitPipeline.hasOwnProperty("shardsPart"), explain.splitPipeline);
assert(explain.splitPipeline.hasOwnProperty("mergerPart"), explain.splitPipeline);
assert(explain.splitPipeline.mergerPart[0], explain.splitPipeline);
assert(explain.splitPipeline.mergerPart[0].hasOwnProperty("$mergeCursors"), explain.splitPipeline);

const mergeCursors = explain.splitPipeline.mergerPart[0]["$mergeCursors"];
assert.eq(mergeCursors.sort, {favColor: 1}, mergeCursors);
assert.eq(mergeCursors.compareWholeSortKey, false, mergeCursors);
assert(!mergeCursors.hasOwnProperty("remotes"), mergeCursors);
assert.eq(mergeCursors.tailableMode, "normal", mergeCursors);
assert.eq(mergeCursors.nss,
          "test.agg_explain_fmt",
          mergeCursors);  // This test manually sets collection and db at the top.
assert.eq(mergeCursors.allowPartialResults, false, mergeCursors);

// Do a sharded explain from a mongod, not mongos, to ensure that it does not have a
// SHARDING_FILTER stage.");
const shardDB = st.shard0.getDB(mongosDB.getName());
explain = shardDB[coll.getName()].explain().aggregate([{$match: {}}]);
assert(!planHasStage(shardDB, explain.queryPlanner.winningPlan, "SHARDING_FILTER"), explain);
st.stop();
}());
