/**
 * Tests that $collStats works as expected when run with the targetAllNodes option turned on and
 * off.
 *
 *  @tags:[
 *      requires_fcv_81,
 *      requires_sharding,
 *  ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const numShards = 2;
const rsNodesPerShardCount = 2;
const st = new ShardingTest({shards: numShards, rs: {nodes: rsNodesPerShardCount}});
const mongos = st.s;
const admin = mongos.getDB("admin");
const config = mongos.getDB("config");
const shards = config.shards.find().toArray();
const collName = "test";
const namespace = jsTestName() + "." + collName;
assert.commandWorked(admin.runCommand({enableSharding: jsTestName(), primaryShard: shards[0]._id}));
assert.commandWorked(admin.adminCommand({shardCollection: namespace, key: {a: 1}}));

function runCollStatsAgg(db, targetAllNodesOption) {
    const targetAllNodes = assert.commandWorked(db.runCommand({
        aggregate: collName,
        pipeline: [{"$collStats": {"targetAllNodes": targetAllNodesOption}}],
        cursor: {}
    }));
    return targetAllNodes.cursor;
}

// Shard collection.
for (let i = 0; i < numShards; i++) {
    assert.commandWorked(st.splitAt(namespace, {a: i + 1}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: namespace, find: {a: i + 2}, to: shards[(i + 1) % numShards]._id}));
}

// Tests targetAllNodes options.
const targetAllNodesFalse = runCollStatsAgg(mongos.getDB(jsTestName()), false).firstBatch;
assert.eq(targetAllNodesFalse.length, numShards);

const targetAllNodesTrue = runCollStatsAgg(mongos.getDB(jsTestName()), true).firstBatch;
assert.eq(targetAllNodesTrue.length, numShards * rsNodesPerShardCount);

st.stop();
