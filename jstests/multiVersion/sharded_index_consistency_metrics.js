/**
 * Tests that the index consistency serverStatus metrics are only tracked when FCV is 4.2.
 */
(function() {
"use strict";

load("jstests/libs/feature_compatibility_version.js");
load("jstests/multiVersion/libs/multi_cluster.js");  // upgradeCluster
load("jstests/multiVersion/libs/multi_rs.js");       // upgradeSet
load("jstests/multiVersion/libs/verify_versions.js");
load("jstests/noPassthrough/libs/sharded_index_consistency_metrics_helpers.js");

// Checking UUID consistency uses cached connections, which are not valid across restarts or
// stepdowns.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// Set up a mixed version cluster with only the config servers running the latest binary.
const intervalMS = 1000;
const st = new ShardingTest({
    shards: 2,
    other: {
        mongosOptions: {binVersion: "last-stable"},
        configOptions: {
            binVersion: "latest",
            setParameter: {"shardedIndexConsistencyCheckIntervalMS": intervalMS}
        },
        rsOptions: {binVersion: "last-stable"},
    },
    rs: {nodes: 3}
});
const dbName = "test";
const ns = dbName + ".foo";

assert.binVersion(st.shard0, "last-stable");
assert.binVersion(st.shard1, "last-stable");
assert.binVersion(st.s0, "last-stable");
checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: "hashed"}}));

//
// Verify the inconsistent indexes counter is never greater than 0 in a mixed version cluster with
// 4.2 binary config servers.
//

checkServerStatusNumCollsWithInconsistentIndexes(st.configRS.getPrimary(), 0);

// Create an inconsistent index and verify it isn't reflected by the counter.
assert.commandWorked(st.shard0.getCollection(ns).createIndex({x: 1}));
sleep(intervalMS * 2);  // Sleep to let the index check run.
checkServerStatusNumCollsWithInconsistentIndexes(st.configRS.getPrimary(), 0);

//
// Verify the counter is 0 until the cluster's FCV is upgraded to 4.2.
//

jsTestLog("Upgrading shard servers.");
st.upgradeCluster("latest", {upgradeMongos: false, upgradeConfigs: false, upgradeShards: true});

jsTestLog("Upgrading mongos servers.");
st.upgradeCluster("latest", {upgradeMongos: true, upgradeConfigs: false, upgradeShards: false});

sleep(intervalMS * 2);  // Sleep to let the index check run.
checkServerStatusNumCollsWithInconsistentIndexes(st.configRS.getPrimary(), 0);

jsTestLog("Upgrading feature compatibility version to latest.");
assert.commandWorked(st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);

// Now the counter should pick up the inconsistency.
checkServerStatusNumCollsWithInconsistentIndexes(st.configRS.getPrimary(), 1);

//
// Verify the counter returns to 0 when the cluster's FCV is downgraded to 4.0.
//

jsTestLog("Downgrading feature compatibility version to last-stable.");
assert.commandWorked(
    st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);

// The count should return to 0, despite the inconsistency remaining.
checkServerStatusNumCollsWithInconsistentIndexes(st.configRS.getPrimary(), 0);

st.stop();
})();
