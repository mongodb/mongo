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
let coll = testDB.range;
let ns = coll.getFullName();
let shardKey = {x: 1};

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, primaryShard.shardName);

jsTest.log("Shard the collection and create chunks.");
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -10}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 10}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 20}}));

jsTest.log("Insert docs (one for each chunk) and check that they end up on the primary shard.");
let docs = [{x: -15}, {x: -5}, {x: 5}, {x: 15}, {x: 25}];
assert.eq(docs.length, findChunksUtil.countChunksForNs(configDB, ns));
assert.commandWorked(coll.insert(docs));
assert.eq(docs.length, primaryShard.getCollection(ns).count());

jsTest.log("Add shards to zones and assign zone key ranges.");
// The chunks on each zone are:
// zoneA: [MinKey, -10)
// zoneB: [-10, 0), [0, 10)
// zoneC: [10, 20), [20, MaxKey)
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zoneA"}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "zoneB"}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: "zoneC"}));
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: ns, min: {x: MinKey}, max: {x: -10}, zone: "zoneA"}));
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: ns, min: {x: -10}, max: {x: 10}, zone: "zoneB"}));
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: ns, min: {x: 10}, max: {x: MaxKey}, zone: "zoneC"}));

jsTest.log("Check that the shards have the assigned zones.");
let shardTags = {
    [st.shard0.shardName]: ["zoneA"],
    [st.shard1.shardName]: ["zoneB"],
    [st.shard2.shardName]: ["zoneC"]
};
assertShardTags(configDB, shardTags);

jsTest.log("Check that the balancer does not balance if \"noBalance\" is true.");
assert.commandWorked(
    configDB.collections.update({_id: ns}, {$set: {"noBalance": true}}, {upsert: true}));
runBalancer(st, 4);
let shardChunkBounds = {
    [primaryShard.shardName]: [
        [{x: MinKey}, {x: -10}],
        [{x: -10}, {x: 0}],
        [{x: 0}, {x: 10}],
        [{x: 10}, {x: 20}],
        [{x: 20}, {x: MaxKey}]
    ]
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assert.eq(docs.length, primaryShard.getCollection(ns).count());

jsTest.log(
    "Let the balancer do the balancing, and check that the chunks and the docs are on the right shards.");
assert.commandWorked(
    configDB.collections.update({_id: ns}, {$set: {"noBalance": false}}, {upsert: true}));
runBalancer(st, 4);
shardChunkBounds = {
    [st.shard0.shardName]: [[{x: MinKey}, {x: -10}]],
    [st.shard1.shardName]: [[{x: -10}, {x: 0}], [{x: 0}, {x: 10}]],
    [st.shard2.shardName]: [[{x: 10}, {x: 20}], [{x: 20}, {x: MaxKey}]]
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log("Test shard's zone changes...");

jsTest.log("Check that removing the only shard that a zone belongs to is not allowed.");
assert.commandFailedWithCode(
    st.s.adminCommand({removeShardFromZone: st.shard0.shardName, zone: "zoneA"}),
    ErrorCodes.ZoneStillInUse);

jsTest.log(
    "Check that removing a zone from a shard causes its chunks and documents to move to other" +
    " shards that the zone belongs to.");
moveZoneToShard(st, "zoneA", st.shard0, st.shard1);
shardTags = {
    [st.shard0.shardName]: [],
    [st.shard1.shardName]: ["zoneB", "zoneA"],
    [st.shard2.shardName]: ["zoneC"]
};
assertShardTags(configDB, shardTags);

runBalancer(st, 1);
shardChunkBounds = {
    [st.shard0.shardName]: [],
    [st.shard1.shardName]: [[{x: MinKey}, {x: -10}], [{x: -10}, {x: 0}], [{x: 0}, {x: 10}]],
    [st.shard2.shardName]: [[{x: 10}, {x: 20}], [{x: 20}, {x: MaxKey}]]
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log("Check that the balancer balances chunks within zones.");
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zoneB"}));
shardTags = {
    [st.shard0.shardName]: ["zoneB"],
    [st.shard1.shardName]: ["zoneB", "zoneA"],
    [st.shard2.shardName]: ["zoneC"]
};
assertShardTags(configDB, shardTags);

runBalancer(st, 1);
shardChunkBounds = {
    [st.shard0.shardName]: [[{x: -10}, {x: 0}]],
    [st.shard1.shardName]: [[{x: MinKey}, {x: -10}], [{x: 0}, {x: 10}]],
    [st.shard2.shardName]: [[{x: 10}, {x: 20}], [{x: 20}, {x: MaxKey}]]
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log("Make another zone change, and check that the chunks and docs are on the right shards.");
assert.commandWorked(st.s.adminCommand({removeShardFromZone: st.shard0.shardName, zone: "zoneB"}));
moveZoneToShard(st, "zoneC", st.shard2, st.shard0);
moveZoneToShard(st, "zoneA", st.shard1, st.shard2);
shardTags = {
    [st.shard0.shardName]: ["zoneC"],
    [st.shard1.shardName]: ["zoneB"],
    [st.shard2.shardName]: ["zoneA"]
};
assertShardTags(configDB, shardTags);

runBalancer(st, 4);
shardChunkBounds = {
    [st.shard0.shardName]: [[{x: 10}, {x: 20}], [{x: 20}, {x: MaxKey}]],
    [st.shard1.shardName]: [[{x: -10}, {x: 0}], [{x: 0}, {x: 10}]],
    [st.shard2.shardName]: [[{x: MinKey}, {x: -10}]]
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log("Test chunk's zone changes...");

// Make a series of zone range changes to make zoneA (and later also zoneB) own only
// chunks that contains no docs. Each time the balancer is expected to split the
// affected chunks and move the chunks and docs that no longer belong to the updated
// zone to the shards with zone that the chunks belong to.

jsTest.log("Assign the key range in zoneA that contains chunks to zoneB, and check that the " +
           "chunks and docs are on the right shards.");
updateZoneKeyRange(st, ns, "zoneA", [{x: MinKey}, {x: -10}], [{x: MinKey}, {x: -20}]);
updateZoneKeyRange(st, ns, "zoneB", [{x: -10}, {x: 10}], [{x: -20}, {x: 10}]);
runBalancer(st, 1);
shardChunkBounds = {
    [st.shard0.shardName]: [[{x: 10}, {x: 20}], [{x: 20}, {x: MaxKey}]],
    [st.shard1.shardName]: [[{x: -20}, {x: -10}], [{x: -10}, {x: 0}], [{x: 0}, {x: 10}]],
    [st.shard2.shardName]: [[{x: MinKey}, {x: -20}]]  // no docs
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log("Assign the key range in zoneB that contains chunks to zoneC, and check that the " +
           "chunks and docs are on the right shards.");
updateZoneKeyRange(st, ns, "zoneB", [{x: -20}, {x: 10}], [{x: -20}, {x: -15}]);
updateZoneKeyRange(st, ns, "zoneC", [{x: 10}, {x: MaxKey}], [{x: -15}, {x: MaxKey}]);
runBalancer(st, 3);
shardChunkBounds = {
    [st.shard0.shardName]: [
        [{x: -15}, {x: -10}],
        [{x: -10}, {x: 0}],
        [{x: 0}, {x: 10}],
        [{x: 10}, {x: 20}],
        [{x: 20}, {x: MaxKey}]
    ],
    [st.shard1.shardName]: [[{x: -20}, {x: -15}]],    // no docs
    [st.shard2.shardName]: [[{x: MinKey}, {x: -20}]]  // no docs
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);
assert.eq(docs.length, st.shard0.getCollection(ns).count());

st.stop();
})();
