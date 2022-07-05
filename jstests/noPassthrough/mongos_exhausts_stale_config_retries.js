// Verifies mongos returns StaleConfig if it exhausts its allowed stale version retry attempts,
// using the command read and write modes.
//
// @tags: [
//   requires_sharding,
// ]
(function() {
"use strict";

const dbName = "test";
const collName = "foo";
const ns = dbName + '.' + collName;

const st = new ShardingTest({shards: 2, config: 1});
const testDB = st.s.getDB(dbName);

// Shard a collection with the only chunk on shard0.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

const sourcePrimary = st.rs0.getPrimary();
const recipientPrimary = st.rs1.getPrimary();

// Disable the best-effort recipient metadata refresh after migrations and move the chunk
// between shards so the recipient shard, shard1, is stale.
assert.commandWorked(recipientPrimary.adminCommand(
    {configureFailPoint: "migrationRecipientFailPostCommitRefresh", mode: "alwaysOn"}));

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

// Disable metadata refreshes on the recipient shard so it will indefinitely return StaleConfig.
assert.commandWorked(recipientPrimary.adminCommand(
    {configureFailPoint: "skipShardFilteringMetadataRefresh", mode: "alwaysOn"}));

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

assert.commandWorked(recipientPrimary.adminCommand(
    {configureFailPoint: "migrationRecipientFailPostCommitRefresh", mode: "off"}));

assert.commandWorked(recipientPrimary.adminCommand(
    {configureFailPoint: "skipShardFilteringMetadataRefresh", mode: "off"}));

st.stop();
})();
