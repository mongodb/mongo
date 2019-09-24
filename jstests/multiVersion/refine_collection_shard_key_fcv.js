// Verifies refineCollectionShardKey can only be run when a cluster's FCV is 4.4.

(function() {
"use strict";

const dbName = "test";
const collName = "foo";
const ns = dbName + '.' + collName;

const st = new ShardingTest({shards: 1});
const configAdminDB = st.configRS.getPrimary().getDB("admin");

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Create an index that can be used for the following shard key refines.
assert.commandWorked(st.s.getCollection(ns).createIndex({_id: 1, x: 1, y: 1}));

// Refining a shard key succeeds in FCV 4.4.
checkFCV(configAdminDB, latestFCV);
assert.commandWorked(st.s.adminCommand({refineCollectionShardKey: ns, key: {_id: 1, x: 1}}));

// Refining a shard key fails in FCV 4.2.
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
checkFCV(configAdminDB, lastStableFCV);
assert.commandFailedWithCode(
    st.s.adminCommand({refineCollectionShardKey: ns, key: {_id: 1, x: 1, y: 1}}),
    ErrorCodes.CommandNotSupported);

// Refining a shard key succeeds after upgrading back to FCV 4.4.
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(configAdminDB, latestFCV);
assert.commandWorked(st.s.adminCommand({refineCollectionShardKey: ns, key: {_id: 1, x: 1, y: 1}}));

st.stop();
}());
