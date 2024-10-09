// Verifies mongos returns StaleConfig if it exhausts its allowed stale version retry attempts,
// using the command read and write modes.
//
// @tags: [
//   requires_sharding,
// ]

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + '.' + collName;

const st = new ShardingTest({shards: 2, config: 1});
const testDB = st.s.getDB(dbName);

// Shard a collection with the only chunk on shard0.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

const recipientPrimary = st.rs1.getPrimary();

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

// Make metadata refreshes on the recipient shard indefinitely return StaleConfig.
let fp = configureFailPoint(recipientPrimary, "alwaysThrowStaleConfigInfo");

// Test various read and write commands that are sent with shard versions and thus can return
// StaleConfig. Batch writes, i.e. insert/update/delete, return batch responses with ok:1 and
// NoProgressMade write errors when retries are exhausted, so they are excluded.
const kCommands = [
    {aggregate: collName, pipeline: [], cursor: {}},
    {count: collName},
    {distinct: collName, query: {}, key: "_id"},
    {find: collName},
    {findAndModify: collName, query: {_id: 0}, update: {$set: {x: 1}}},
];

kCommands.forEach((cmd) => {
    // The recipient shard should return StaleConfig until mongos exhausts its retries and
    // returns the final StaleConfig to the client.
    assert.commandFailedWithCode(testDB.runCommand(cmd),
                                 ErrorCodes.StaleConfig,
                                 "expected to fail with StaleConfig, cmd: " + tojson(cmd));
});

fp.off();

if (jsTest.options().storageEngine === "inMemory") {
    jsTestLog("Skipping the last scenario because we need persistance to restart nodes");
    st.stop();
    quit();
}

// Restart nodes to clear filtering metadata to trigger a refresh with following operations.
st.rs0.nodes.forEach(node => {
    st.rs0.restart(node, undefined, undefined, false /* wait */);
});

// Waits for a stable primary.
st.rs0.getPrimary();

// Enable failpoint to force refresh fail until exhausting mongos retries.
fp = configureFailPoint(st.shard0, "skipShardFilteringMetadataRefresh");

// Test that StaleConfig is not bubble up in case of a failed refreshed.
assert.commandFailedWithCode(testDB.createCollection("coll3"), ErrorCodes.InternalError);
assert.commandFailedWithCode(testDB.collName.insert({x: 10}), ErrorCodes.NoProgressMade);

fp.off();

st.stop();
