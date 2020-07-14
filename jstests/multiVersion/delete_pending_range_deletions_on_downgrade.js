(function() {
"use strict";

load("jstests/libs/uuid_util.js");

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const rangeDeletionNs = "config.rangeDeletions";

// Create 2 shards with 3 replicas each.
let st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

// Create a sharded collection with two chunks: [-inf, 50), [50, inf)
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

// Pause range deletion.
let originalShard0Primary = st.rs0.getPrimary();
originalShard0Primary.adminCommand({configureFailPoint: 'suspendRangeDeletion', mode: 'alwaysOn'});

// Write range to deletion collection
let deletionTask = {
    _id: UUID(),
    nss: ns,
    collectionUuid: UUID(),
    donorShardId: "unused",
    range: {min: {x: 50}, max: {x: MaxKey}},
    whenToClean: "now",
    // Mark the range as pending, otherwise the task will be processed immediately on being
    // inserted (and deleted after it's proessed) rather than being deleted on setFCV downgrade.
    pending: true
};

let deletionsColl = st.shard0.getCollection(rangeDeletionNs);

// Write range to deletion collection
assert.commandWorked(deletionsColl.insert(deletionTask));

// Verify deletion count.
assert.eq(deletionsColl.find().itcount(), 1);

print("setting fcv: " + lastLTSFCV);
assert.commandWorked(st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
checkFCV(st.shard0.getDB("admin"), lastLTSFCV);

// Verify deletion count.
assert.eq(deletionsColl.find().itcount(), 0);

st.stop();
})();
