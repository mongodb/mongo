/**
 * Test the behavior of the $unionWith aggregation stage against a standalone/sharded cluster during
 * upgrade.
 *
 *  Checking UUID consistency uses cached connections, which are not valid across restarts or
 *  stepdowns.
 */
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";

load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster.

let conn = MongoRunner.runMongod({binVersion: "latest"});
assert.neq(null, conn, "mongod was unable to start up");
let testDB = conn.getDB(jsTestName());

// Set the feature compatibility version to the last-stable version.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// Seed the two involved collections.
assert.commandWorked(testDB.collA.insert({fromA: 1}));
assert.commandWorked(testDB.collB.insert({fromB: 1}));

// Verify that we can still use $unionWith since the binary version is 4.4.
const pipeline = [{$unionWith: "collB"}, {$project: {_id: 0}}];
assert.sameMembers([{fromA: 1}, {fromB: 1}], testDB.collA.aggregate(pipeline).toArray());

// Set the feature compatibility version to the latest version.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Verify that we can still use $unionWith.
assert.sameMembers([{fromA: 1}, {fromB: 1}], testDB.collA.aggregate(pipeline).toArray());

MongoRunner.stopMongod(conn);

// Start a sharded cluster in which all mongod and mongos processes are "last-stable" binVersion.
let st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2, binVersion: "last-stable"},
    other: {mongosOptions: {binVersion: "last-stable"}}
});

testDB = st.s.getDB(jsTestName());
assert.commandWorked(testDB.runCommand({create: "collA"}));
st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);

// Seed the two involved collections.
assert.commandWorked(testDB.collA.insert({fromA: 1}));
assert.commandWorked(testDB.collB.insert({fromB: 1}));

// Aggregations with $unionWith should fail against older binary versions.
assert.commandFailedWithCode(
    testDB.runCommand({aggregate: "collA", pipeline: pipeline, cursor: {}}), 40324);

// Upgrade the config servers and the shards to the "latest" binVersion.
st.upgradeCluster("latest", {upgradeShards: true, upgradeConfigs: true, upgradeMongos: false});

// Since mongos is still on 4.2, $unionWith should fail to parse.
assert.commandFailedWithCode(
    testDB.runCommand({aggregate: "collA", pipeline: pipeline, cursor: {}}), 40324);

// Upgrade mongos to the "latest" binVersion but keep the old FCV.
st.upgradeCluster("latest", {upgradeShards: false, upgradeConfigs: false, upgradeMongos: true});
testDB = st.s.getDB(jsTestName());

// Now an aggregation containing $unionWith should pass because all nodes are on binary version 4.4.
assert.sameMembers([{fromA: 1}, {fromB: 1}], testDB.collA.aggregate(pipeline).toArray());

// For completeness, set the FCV to the latest.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Verify that $unionWith is allowed in a fully upgraded cluster.
assert.sameMembers([{fromA: 1}, {fromB: 1}], testDB.collA.aggregate(pipeline).toArray());

st.stop();
}());
