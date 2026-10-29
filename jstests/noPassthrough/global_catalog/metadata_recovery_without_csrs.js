/**
 * Tests that shards can recover their sharding metadata from the persisted authoritative collections
 * and can continue to answer user queries targeting a sharded collection even when they cannot reach
 * the config server replica set (CSRS).
 *
 * Rather than shutting down the CSRS (which would force the router to reload its ShardRegistry from
 * an unreachable config server, and which would also prevent shard startup from completing), this
 * test uses mongobridge to partition the shards from the CSRS *after* they have restarted. The
 * router keeps its connection to the CSRS, so from its point of view nothing has changed and it
 * routes queries normally; the shards must answer those queries using only their recovered cached
 * metadata.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_persistence,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Some teardown consistency checks talk to the shards while they are still partitioned from the
// config server; skip the ones that would depend on that connectivity.
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckMetadataConsistency = true;

const st = new ShardingTest({shards: 2, useBridge: true});

const dbName = "test";
const collName = "recovery";
const ns = dbName + "." + collName;

const mongos = st.s0;
// Create a sharded collection and spread its data across two different shards.
assert.commandWorked(
    mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

const coll = mongos.getDB(dbName).getCollection(collName);
assert.commandWorked(coll.insert([{_id: -2}, {_id: -1}, {_id: 1}, {_id: 2}]));

// Sanity check that data really lives on both shards.
assert.eq(2, st.shard0.getCollection(ns).countDocuments({}));
assert.eq(2, st.shard1.getCollection(ns).countDocuments({}));

// Restart both shards. The CSRS is still reachable here, which lets shard startup complete (a
// shard-only node blocks on loading global settings from the config server during startup).
jsTest.log.info("Restarting shards");
st.restartShardRS(0, true /* waitForPrimary */);
st.restartShardRS(1, true /* waitForPrimary */);

// Partition every shard node from every config server node in both directions. This simulates the
// CSRS being unavailable to the shards while leaving the router's connection to the CSRS intact.
jsTest.log.info("Partitioning shards from the CSRS");
const shardNodes = [...st.rs0.nodes, ...st.rs1.nodes];
const configNodes = st.configRS.nodes;
for (const shardNode of shardNodes) {
    for (const configNode of configNodes) {
        shardNode.discardMessagesFrom(configNode, 1.0);
        configNode.discardMessagesFrom(shardNode, 1.0);
    }
}

// The shards should answer user queries routed by the router using only their recovered metadata,
// without needing to reach the config server.
jsTest.log.info(
    "Verifying queries succeed against the sharded collection while shards cannot reach the CSRS",
);

// Broadcast query hitting both shards.
assert.eq(4, coll.find().itcount());

// Targeted queries, one for each shard.
assert.eq(1, coll.find({_id: -1}).itcount());
assert.eq(1, coll.find({_id: 1}).itcount());

// Heal the partition before tearing down so that teardown consistency checks can run.
jsTest.log.info("Healing the partition before teardown");
for (const shardNode of shardNodes) {
    for (const configNode of configNodes) {
        shardNode.discardMessagesFrom(configNode, 0.0);
        configNode.discardMessagesFrom(shardNode, 0.0);
    }
}

st.stop();
