/**
 * Test that chunks and documents are moved correctly after zone changes.
 */
(function() {
'use strict';

load("jstests/sharding/libs/zone_changes_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

let st = new ShardingTest({shards: 3});
let primaryShard = st.shard0;
let dbName = "test";
let testDB = st.s.getDB(dbName);
let configDB = st.s.getDB("config");
let coll = testDB.compound;
let ns = coll.getFullName();
let shardKey = {x: 1, y: 1};

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, primaryShard.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));

jsTest.log("Insert docs and check that they end up on the primary shard.");
let docs = [{x: -10, y: -10}, {x: -1, y: -1}, {x: 0, y: 0}];
assert.commandWorked(coll.insert(docs));
assert.eq(1, findChunksUtil.countChunksForNs(configDB, ns));
assert.eq(docs.length, primaryShard.getCollection(ns).count());

jsTest.log("Add shards to zones and assign zone key ranges.");
// The chunks on each zone after balancing should be:
// zoneA:  x: [0, MaxKey), y: [0, MaxKey)
// zoneB:  x: [MinKey, 0), y: [MinKey, 0)
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zoneA"}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "zoneB"}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: {x: MinKey, y: MinKey}, max: {x: 0, y: 0}, zone: "zoneA"}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: {x: 0, y: 0}, max: {x: MaxKey, y: MaxKey}, zone: "zoneB"}));

jsTest.log("Check that the shards have the assigned zones.");
let shardTags = {[st.shard0.shardName]: ["zoneA"], [st.shard1.shardName]: ["zoneB"]};
assertShardTags(configDB, shardTags);

jsTest.log(
    "Check that the balancer splits the chunks based on the zone ranges and move the chunks" +
    " and docs to the right shards.");
runBalancer(st, 2);
let shardChunkBounds = {
    [st.shard0.shardName]: [[{x: MinKey, y: MinKey}, {x: 0, y: 0}]],
    [st.shard1.shardName]: [[{x: 0, y: 0}, {x: MaxKey, y: MaxKey}]]
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log("Test shard's zone changes...");

jsTest.log(
    "Check that removing a zone from a shard causes its chunks and documents to move to other" +
    " shards that the zone belongs to.");
moveZoneToShard(st, "zoneB", st.shard1, st.shard0);
shardTags = {
    [st.shard0.shardName]: ["zoneA", "zoneB"],
    [st.shard1.shardName]: [],
};
assertShardTags(configDB, shardTags);

runBalancer(st, 1);
shardChunkBounds = {
    [st.shard0.shardName]:
        [[{x: MinKey, y: MinKey}, {x: 0, y: 0}], [{x: 0, y: 0}, {x: MaxKey, y: MaxKey}]],
    [st.shard1.shardName]: []
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log("Test chunk's zone changes...");

jsTest.log("Make one of the chunks not aligned with zone ranges.");
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "zoneC"}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: {x: MinKey, y: MinKey}, max: {x: 0, y: 0}, zone: null}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: {x: MinKey, y: MinKey}, max: {x: -5, y: -10}, zone: "zoneA"}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: {x: -5, y: -10}, max: {x: 0, y: 0}, zone: "zoneC"}));
shardTags = {
    [st.shard0.shardName]: ["zoneA", "zoneB"],
    [st.shard1.shardName]: ["zoneC"],
};
assertShardTags(configDB, shardTags);

jsTest.log(
    "Check that the balancer splits the chunk and that all chunks and docs are on the right shards.");
runBalancer(st, 1);
shardChunkBounds = {
    [st.shard0.shardName]:
        [[{x: MinKey, y: MinKey}, {x: -5, y: -10}], [{x: 0, y: 0}, {x: MaxKey, y: MaxKey}]],
    [st.shard1.shardName]: [[{x: -5, y: -10}, {x: 0, y: 0}]]
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

st.stop();
})();
