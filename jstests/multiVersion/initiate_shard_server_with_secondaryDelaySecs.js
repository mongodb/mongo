/**
 * Tests that new shard servers using the 'latest' binary in 'lastLTSFCV' are able to use the
 * 'secondaryDelaySecs' field. Once the shard is added to a sharded cluster, it must abide by
 * the FCV restrictions of that cluster.
 *
 * TODO SERVER-55128: Remove this test once 5.0 becomes 'lastLTS'.
 */

(function() {
"use strict";

function checkForSecondaryDelaySecs(primaryDB) {
    let config = primaryDB.runCommand({replSetGetConfig: 1}).config;
    assert.eq(config.members[0].secondaryDelaySecs, 0, config);
    assert(!config.members[0].hasOwnProperty('slaveDelay'), config);
}

function checkForSlaveDelaySecs(primaryDB) {
    let config = primaryDB.runCommand({replSetGetConfig: 1}).config;
    assert.eq(config.members[0].slaveDelay, 0, config);
    assert(!config.members[0].hasOwnProperty('secondaryDelaySecs'), config);
}

// A 'latest' binary cluster started with clean data files will initialize shard servers
// with 'lastLTSFCV' and then use addShard to get them to 'latestFCV'.
const st = new ShardingTest({
    shards: {rs0: {nodes: [{binVersion: "latest"}]}},
});
const mongosAdminDB = st.s.getDB("admin");
const configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
const shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");

checkFCV(configPrimaryAdminDB, latestFCV);
checkFCV(shardPrimaryAdminDB, latestFCV);

jsTestLog("Test adding a shard server to cluster running 'latest' binaries and 'latestFCV'");
// A 'latest' binary replica set started as a shard server defaults to 'lastLTSFCV'. Allow
// the use of 'secondaryDelaySecs' before the shard server is added to a cluster.
let shard2 = new ReplSetTest({
    name: "shard2",
    nodes: [{binVersion: "latest", rsConfig: {secondaryDelaySecs: 0}}],
    nodeOptions: {shardsvr: ""},
    useHostName: true
});

shard2.startSet();
shard2.initiate();
let latestShardPrimaryAdminDB = shard2.getPrimary().getDB("admin");
checkFCV(latestShardPrimaryAdminDB, lastLTSFCV);
// Even though shard2 is in the 'lastLTSFCV', its config should still have the
// 'secondaryDelaySecs' field.
let config = checkForSecondaryDelaySecs(latestShardPrimaryAdminDB);

// The cluster is in the 'latestFCV', so adding shard2 to the cluster should update shard2's
// FCV.
assert.commandWorked(mongosAdminDB.runCommand({addShard: shard2.getURL()}));
checkFCV(latestShardPrimaryAdminDB, latestFCV);

// FCV can be set to 'lastLTSFCV' on mongos.
assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// featureCompatibilityVersion propagates to the config, existing shard, and newly added shard.
checkFCV(configPrimaryAdminDB, lastLTSFCV);
checkFCV(shardPrimaryAdminDB, lastLTSFCV);
checkFCV(latestShardPrimaryAdminDB, lastLTSFCV);

// The FCV was downgraded to 'lastLTSFCV', which is 4.4 in this case. During FCV downgrade, the
// 'secondaryDelaySecs' will automatically be changed to 'slaveDelay'.
checkForSlaveDelaySecs(latestShardPrimaryAdminDB);
checkForSlaveDelaySecs(configPrimaryAdminDB);
checkForSlaveDelaySecs(shardPrimaryAdminDB);

jsTestLog("Test adding a shard server to cluster running 'latest' binaries and 'lastLTSFCV'");
// A 'latest' binary replica set started as a shard server defaults to 'lastLTSFCV'. Allow
// the use of 'secondaryDelaySecs' before the shard server is added to a cluster.
let shard3 = new ReplSetTest({
    name: "shard3",
    nodes: [{binVersion: "latest", rsConfig: {secondaryDelaySecs: 0}}],
    nodeOptions: {shardsvr: ""},
    useHostName: true
});

shard3.startSet();
shard3.initiate();
latestShardPrimaryAdminDB = shard3.getPrimary().getDB("admin");
checkFCV(latestShardPrimaryAdminDB, lastLTSFCV);
// Even though shard3 is in the 'lastLTSFCV', its config should still have the
// 'secondaryDelaySecs' field.
checkForSecondaryDelaySecs(latestShardPrimaryAdminDB);

// The cluster is also in the 'lastLTSFCV', so adding shard3 to a downgraded cluster will not
// update shard3's FCV.
assert.commandWorked(mongosAdminDB.runCommand({addShard: shard3.getURL()}));
checkFCV(latestShardPrimaryAdminDB, lastLTSFCV);
// Since the FCV was not updated, secondaryDelaySecs is still present in the config.
checkForSecondaryDelaySecs(latestShardPrimaryAdminDB);

// The cluster wide FCV is 'lastLTSFCV', which is 4.4 in this case. Despite replSetGetConfig
// returning 'secondaryDelaySecs' for shard3, future reconfigs on shard3  must use 'slaveDelay'.
config = latestShardPrimaryAdminDB.runCommand({replSetGetConfig: 1}).config;
config.version++;
assert.commandFailedWithCode(latestShardPrimaryAdminDB.runCommand({replSetReconfig: config}),
                             ErrorCodes.NewReplicaSetConfigurationIncompatible);

delete config.members[0].secondaryDelaySecs;
config.members[0].slaveDelay = 0;
config.version++;

// Test that a reconfig with 'slaveDelay' succeeds.
assert.commandWorked(latestShardPrimaryAdminDB.runCommand({replSetReconfig: config}));
checkForSlaveDelaySecs(latestShardPrimaryAdminDB);

// FCV can be set to 'latestFCV' on mongos.
assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);
checkFCV(shardPrimaryAdminDB, latestFCV);
checkFCV(latestShardPrimaryAdminDB, latestFCV);

checkForSecondaryDelaySecs(latestShardPrimaryAdminDB);
checkForSecondaryDelaySecs(configPrimaryAdminDB);
checkForSecondaryDelaySecs(shardPrimaryAdminDB);

// Call ShardingTest.stop before shutting down shard2 and shard3, so that the UUID check in
// ShardingTest.stop can talk to them.
st.stop();
shard2.stopSet();
shard3.stopSet();
})();
