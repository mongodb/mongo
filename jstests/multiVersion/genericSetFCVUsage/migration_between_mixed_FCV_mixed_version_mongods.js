/**
 * Test that it is not possible to move a chunk from an upgrade featureCompatibilityVersion node to
 * a downgrade binary version node.
 */

(function() {
"use strict";

let st = new ShardingTest({
    shards: [{binVersion: "latest"}, {binVersion: "last-stable"}],
    mongos: {binVersion: "latest"},
    other: {shardAsReplicaSet: false},
});

let testDB = st.s.getDB("test");

// Create a sharded collection with primary shard 0.
assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName()}));
st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: testDB.coll.getFullName(), key: {a: 1}}));

// Set the featureCompatibilityVersion to latestFCV. This will fail because the
// featureCompatibilityVersion cannot be set to latestFCV on shard 1, but it will set the
// featureCompatibilityVersion to latestFCV on shard 0.
assert.commandFailed(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV, latestFCV);
checkFCV(st.shard0.getDB("admin"), latestFCV);
checkFCV(st.shard1.getDB("admin"), lastStableFCV);

// It is not possible to move a chunk from a latestFCV shard to a last-stable binary version
// shard.
assert.commandFailedWithCode(
    st.s.adminCommand(
        {moveChunk: testDB.coll.getFullName(), find: {a: 1}, to: st.shard1.shardName}),
    ErrorCodes.IncompatibleServerVersion);

st.stop();
})();
