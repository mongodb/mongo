// Tests that $group pushdown to SBE feature works in a sharded environment for some special
// scenarios.
//
// Notes:
// - In a sharded environment, the mongos splits a $group stage into two different stages. One is a
// merge $group stage at the mongos-side which does the global aggregation and the other is a $group
// stage at the shard-side which does the partial aggregation.
// - All aggregation features are tested by aggregation test suites under a sharded environment
// through passthrough tests. So, this test suite focuses on some special scenarios like for
// example, $group is pushed down to SBE at the shard-side and some accumulators may return the
// partial aggregation results in a special format to the mongos.
//
// Needs the following tag to be excluded from linux-64-duroff build variant because running
// wiredTiger without journaling in a replica set is not supported.
// @tags: [requires_sharding]
(function() {
'use strict';

load("jstests/libs/analyze_plan.js");

// As of now, $group pushdown to SBE feature is not enabled by default. So, enables it with a
// minimal configuration of a sharded cluster.
//
// TODO Remove {setParameter: "featureFlagSBEGroupPushdown=true"} when the feature is enabled by
// default.
const st = new ShardingTest(
    {config: 1, shards: 1, shardOptions: {setParameter: "featureFlagSBEGroupPushdown=true"}});

// This database name can provide multiple similar test cases with a good separate namespace and
// each test case may create a separate collection for its own dataset.
const db = st.getDB(jsTestName());
const dbAtShard = st.shard0.getDB(jsTestName());

// Makes sure that $group pushdown to SBE feature is enabled.
assert(
    assert.commandWorked(dbAtShard.adminCommand({getParameter: 1, featureFlagSBEGroupPushdown: 1}))
        .featureFlagSBEGroupPushdown.value);

// Makes sure that the test db is sharded and the data is stored into the only shard.
assert.commandWorked(st.s0.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);

// A test case for a sharded $sum: Verifies that $group with $sum pushed down to SBE works in a
// sharded environment.

let coll = db.partial_sum;

// Makes sure that the collection is sharded.
assert.commandWorked(st.s0.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

// Prepares data for the 'NumberLong' sum result to overflow, when the shard sends back the partial
// sum as a doc with 'subTotal' and 'subTotalError' fields to the mongos. All data go to the only
// shard and so overflow will happen.
assert.commandWorked(
    coll.insert([{a: 1, b: NumberLong("9223372036854775807")}, {a: 2, b: NumberLong("10")}]));

// Turns to the classic engine at the shard before figuring out its result.
assert.commandWorked(
    dbAtShard.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));

// Collects the classic engine's result as the expected result, executing the pipeline at the
// mongos.
const pipeline1 = [{$group: {_id: "$a", s: {$sum: "$b"}}}];
const classicalRes1 =
    coll.runCommand({aggregate: coll.getName(), pipeline: pipeline1, cursor: {}}).cursor.firstBatch;

// Collects the classic engine's result as the expected result, executing the pipeline at the
// mongos.
const pipeline2 = [{$group: {_id: null, s: {$sum: "$b"}}}];
const classicalRes2 =
    coll.runCommand({aggregate: coll.getName(), pipeline: pipeline2, cursor: {}}).cursor.firstBatch;

// Turns to the SBE engine at the shard.
assert.commandWorked(
    dbAtShard.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));

// Verifies that the SBE engine's results are same as the expected results, executing the pipeline
// at the mongos.
const sbeRes1 =
    coll.runCommand({aggregate: coll.getName(), pipeline: pipeline1, cursor: {}}).cursor.firstBatch;
assert.sameMembers(sbeRes1, classicalRes1);

const sbeRes2 =
    coll.runCommand({aggregate: coll.getName(), pipeline: pipeline2, cursor: {}}).cursor.firstBatch;
assert.sameMembers(sbeRes2, classicalRes2);

st.stop();
}());
