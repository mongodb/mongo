/**
 * Tests the serverStatus opcounters for the 'aggregate' command in a sharded cluster.
 *
 * Verifies that aggregate increments opcounters.aggregates on both mongos and the shard, and
 * that the counter is exclusive from opcounters.queries on both.
 *
 * Also verifies that $lookup and $unionWith sub-pipelines targeting a different sharded collection
 * increment the shard-level aggregate counter for each shard that executes a sub-pipeline, while
 * mongos still only counts the original user-facing command once.
 *
 * See jstests/noPassthroughWithMongod/query/opcounters_aggregate.js for the single-node variant
 * with more exhaustive semantic coverage.
 *
 * @tags: [
 *   # The config fuzzer may run logical session cache refreshes in the background, which modifies
 *   # some serverStatus metrics read in this test.
 *   does_not_support_config_fuzzer,
 *   inspects_command_opcounters,
 *   does_not_support_repeated_reads,
 *   # The aggregate opcounters were added for 9.0.
 *   requires_fcv_90,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 1}});

const mongosDB = st.s.getDB("test");
const shard0DB = st.shard0.getDB("test");
const shard1DB = st.shard1.getDB("test");
const coll = mongosDB.getCollection(jsTestName());
assert.commandWorked(st.s.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.shard0.shardName}));

coll.drop();
assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));

function getMongosCounters() {
    return mongosDB.serverStatus().opcounters;
}

function getShard0Counters() {
    return shard0DB.serverStatus().opcounters;
}

function getShard1Counters() {
    return shard1DB.serverStatus().opcounters;
}

// --- aggregate increments opcounters.aggregates on both mongos and shard ---
// gotAggregate() is only ever called from the aggregate command itself — there are no internal or
// background processes that fire this counter — so exact-delta assertions are safe here.
//
// coll is unsharded and lives on shard0 (the primary shard). shard1 sees no traffic here.

let routerBefore = getMongosCounters();
let shardBefore = getShard0Counters();

coll.aggregate([{$match: {}}]).toArray();

let routerAfter = getMongosCounters();
let shardAfter = getShard0Counters();

assert.eq(routerBefore.aggregate + 1, routerAfter.aggregate, "mongos: aggregate counter should increment");
assert.eq(routerBefore.query, routerAfter.query, "mongos: aggregate should NOT increment queries counter");

assert.gte(shardAfter.aggregate, shardBefore.aggregate + 1, "shard: aggregate counter should increment");
assert.eq(shardBefore.query, shardAfter.query, "shard: aggregate should NOT increment queries counter");

// --- find increments queries but NOT aggregates on both mongos and shard ---

routerBefore = getMongosCounters();
shardBefore = getShard0Counters();

coll.find({}).toArray();

routerAfter = getMongosCounters();
shardAfter = getShard0Counters();

assert.eq(routerBefore.query + 1, routerAfter.query, "mongos: find should increment queries counter");
assert.eq(routerBefore.aggregate, routerAfter.aggregate, "mongos: find should NOT increment aggregate counter");

assert.eq(shardBefore.query + 1, shardAfter.query, "shard: find should increment queries counter");
// It's too flakey to assert that the find command does NOT modify the shard's aggregate counter,
// since many internal operations use the aggregate command which would increment this. The
// assertion on mongos is enough.

// --- $unionWith and $lookup on a sharded foreign collection increment shard counters multiple
//     times but mongos only once ---
//
// The 'foreign' collection is sharded across both shard0 and shard1. The 'coll' collection
// (the outer pipeline source) is unsharded and lives on shard0 only.
//
// When the user issues a single aggregate command on mongos:
//   - mongos increments its counter exactly once (the user-facing command).
//   - shard0 increments for the outer pipeline execution.
//   - The sub-pipeline (from $unionWith or $lookup) is dispatched as separate aggregate commands
//     to each shard hosting the foreign collection. shard1 therefore receives an aggregate command
//     it would never see from the outer pipeline alone, proving the sub-pipeline generated
//     additional shard-level aggregate traffic.

const foreignColl = mongosDB.getCollection(jsTestName() + "_foreign");
foreignColl.drop();
assert.commandWorked(st.s.adminCommand({enableSharding: mongosDB.getName()}));
assert.commandWorked(
    st.s.adminCommand({
        shardCollection: foreignColl.getFullName(),
        key: {_id: 1},
    }),
);

// Split at _id: 0 and move the upper chunk to shard1 so that foreign is distributed across both
// shards.
assert.commandWorked(st.s.adminCommand({split: foreignColl.getFullName(), middle: {_id: 0}}));
assert.commandWorked(
    st.s.adminCommand({
        moveChunk: foreignColl.getFullName(),
        find: {_id: 1},
        to: st.shard1.shardName,
    }),
);
assert.commandWorked(
    foreignColl.insertMany([
        {_id: -1, val: "neg"},
        {_id: 1, val: "pos"},
    ]),
);

// $unionWith: mongos should count exactly 1; shard1 must count at least 1 (sub-pipeline only).
routerBefore = getMongosCounters();
shardBefore = getShard0Counters();
let s1Before = getShard1Counters();

coll.aggregate([{$unionWith: {coll: foreignColl.getName(), pipeline: []}}]).toArray();

routerAfter = getMongosCounters();
shardAfter = getShard0Counters();
let s1After = getShard1Counters();

assert.eq(
    routerBefore.aggregate + 1,
    routerAfter.aggregate,
    "$unionWith: mongos should count the user command exactly once",
);
assert.gte(
    shardAfter.aggregate,
    shardBefore.aggregate + 1,
    "$unionWith: shard0 should count at least one aggregate (outer pipeline)",
);
assert.gte(
    s1After.aggregate,
    s1Before.aggregate + 1,
    "$unionWith: shard1 should count at least one aggregate from the sub-pipeline " +
        "(it has no outer-pipeline chunks so any increment must be from the sub-pipeline)",
);

// $lookup (pipeline syntax): same invariant.
routerBefore = getMongosCounters();
shardBefore = getShard0Counters();
s1Before = getShard1Counters();

coll.aggregate([
    {
        $lookup: {
            from: foreignColl.getName(),
            pipeline: [{$match: {}}],
            as: "joined",
        },
    },
]).toArray();

routerAfter = getMongosCounters();
shardAfter = getShard0Counters();
s1After = getShard1Counters();

assert.eq(
    routerBefore.aggregate + 1,
    routerAfter.aggregate,
    "$lookup: mongos should count the user command exactly once",
);
assert.gte(
    shardAfter.aggregate,
    shardBefore.aggregate + 1,
    "$lookup: shard0 should count at least one aggregate (outer pipeline)",
);
assert.gte(
    s1After.aggregate,
    s1Before.aggregate + 1,
    "$lookup: shard1 should count at least one aggregate from the sub-pipeline " +
        "(it has no outer-pipeline chunks so any increment must be from the sub-pipeline)",
);

st.stop();
