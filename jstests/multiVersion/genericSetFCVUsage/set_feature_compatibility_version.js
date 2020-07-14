/**
 * Tests setFeatureCompatibilityVersion.
 */

// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// A replset test case checks that replication to a secondary ceases, so we do not expect identical
// data.
TestData.skipCheckDBHashes = true;

(function() {
"use strict";

load("jstests/libs/get_index_helpers.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/rslib.js");

let dbpath = MongoRunner.dataPath + "feature_compatibility_version";
resetDbpath(dbpath);
let res;

const latest = "latest";
const lastLTS = "last-lts";

//
// Standalone tests.
//

let conn;
let adminDB;

// A 'latest' binary standalone should default to 'latestFCV'.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
assert.neq(
    null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
adminDB = conn.getDB("admin");
checkFCV(adminDB, latestFCV);

jsTestLog("EXPECTED TO FAIL: featureCompatibilityVersion cannot be set to an invalid value");
assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: 5}));
assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "4.8"}));
assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));

jsTestLog("EXPECTED TO FAIL: setFeatureCompatibilityVersion rejects unknown fields.");
assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTS, unknown: 1}));

jsTestLog("EXPECTED TO FAIL: setFeatureCompatibilityVersion can only be run on the admin database");
assert.commandFailed(conn.getDB("test").runCommand({setFeatureCompatibilityVersion: lastLTS}));

jsTestLog("EXPECTED TO FAIL: featureCompatibilityVersion cannot be set via setParameter");
assert.commandFailed(adminDB.runCommand({setParameter: 1, featureCompatibilityVersion: lastLTS}));

// setFeatureCompatibilityVersion fails to downgrade FCV if the write fails.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCollectionUpdates",
    data: {collectionNS: "admin.system.version"},
    mode: "alwaysOn"
}));
jsTestLog(
    "EXPECTED TO FAIL: setFeatureCompatibilityVersion fails to downgrade FCV if the write fails");
assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
checkFCV(adminDB, latestFCV);
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCollectionUpdates",
    data: {collectionNS: "admin.system.version"},
    mode: "off"
}));

// featureCompatibilityVersion can be downgraded to 'lastLTSFCV'.
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
checkFCV(adminDB, lastLTSFCV);

// setFeatureCompatibilityVersion fails to upgrade to 'latestFCV' if the write fails.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCollectionUpdates",
    data: {collectionNS: "admin.system.version"},
    mode: "alwaysOn"
}));
jsTestLog(
    "EXPECTED TO FAIL: setFeatureCompatibilityVersion fails to upgrade to 'latestFCV' if the write fails");
assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(adminDB, lastLTSFCV);
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCollectionUpdates",
    data: {collectionNS: "admin.system.version"},
    mode: "off"
}));

// featureCompatibilityVersion can be upgraded to 'latestFCV'.
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(adminDB, latestFCV);

MongoRunner.stopMongod(conn);

// featureCompatibilityVersion is durable.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
assert.neq(
    null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
adminDB = conn.getDB("admin");
checkFCV(adminDB, latestFCV);
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
checkFCV(adminDB, lastLTSFCV);
MongoRunner.stopMongod(conn);

conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
assert.neq(null,
           conn,
           "mongod was unable to start up with binary version=" + latest +
               " and last-lts featureCompatibilityVersion");
adminDB = conn.getDB("admin");
checkFCV(adminDB, lastLTSFCV);
MongoRunner.stopMongod(conn);

// If you upgrade from 'lastLTS' binary to 'latest' binary and have non-local databases, FCV
// remains 'lastLTSFCV'.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: lastLTS});
assert.neq(
    null, conn, "mongod was unable to start up with version=" + lastLTS + " and no data files");
assert.commandWorked(conn.getDB("test").coll.insert({a: 5}));
adminDB = conn.getDB("admin");
checkFCV(adminDB, lastLTSFCV);
MongoRunner.stopMongod(conn);

conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
assert.neq(null,
           conn,
           "mongod was unable to start up with binary version=" + latest +
               " and featureCompatibilityVersion=" + lastLTSFCV);
adminDB = conn.getDB("admin");
checkFCV(adminDB, lastLTSFCV);
MongoRunner.stopMongod(conn);

// A 'latest' binary mongod started with --shardsvr and clean data files defaults to
// 'lastLTSFCV'.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, shardsvr: ""});
assert.neq(
    null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
adminDB = conn.getDB("admin");
checkFCV(adminDB, lastLTSFCV);
MongoRunner.stopMongod(conn);

//
// Replica set tests.
//

let rst;
let rstConns;
let replSetConfig;
let primaryAdminDB;
let secondaryAdminDB;

// 'latest' binary replica set.
rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
rst.startSet();
rst.initiate();
primaryAdminDB = rst.getPrimary().getDB("admin");
secondaryAdminDB = rst.getSecondary().getDB("admin");

// FCV should default to 'latestFCV' on primary and secondary in a 'latest' binary replica set.
checkFCV(primaryAdminDB, latestFCV);
rst.awaitReplication();
checkFCV(secondaryAdminDB, latestFCV);

// featureCompatibilityVersion propagates to secondary.
assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
checkFCV(primaryAdminDB, lastLTSFCV);
rst.awaitReplication();
checkFCV(secondaryAdminDB, lastLTSFCV);

jsTestLog("EXPECTED TO FAIL: setFeatureCompatibilityVersion cannot be run on secondary");
assert.commandFailed(secondaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

rst.stopSet();

// A 'latest' binary secondary with a 'lastLTS' binary primary will have 'lastLTSFCV'
rst = new ReplSetTest({nodes: [{binVersion: lastLTS}, {binVersion: latest}]});
rstConns = rst.startSet();
replSetConfig = rst.getReplSetConfig();
replSetConfig.members[1].priority = 0;
replSetConfig.members[1].votes = 0;
rst.initiate(replSetConfig);
rst.waitForState(rstConns[0], ReplSetTest.State.PRIMARY);
secondaryAdminDB = rst.getSecondary().getDB("admin");
checkFCV(secondaryAdminDB, lastLTSFCV);
rst.stopSet();

// Test that a 'lastLTS' secondary can successfully perform initial sync from a 'latest'
// primary with 'lastLTSFCV'.
rst = new ReplSetTest({
    nodes: [{binVersion: latest}, {binVersion: latest, rsConfig: {priority: 0}}],
    settings: {chainingAllowed: false}
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
primaryAdminDB = primary.getDB("admin");
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

let secondary = rst.getSecondary();

// The command should fail because wtimeout expires before a majority responds.
stopServerReplication(secondary);
res = primary.adminCommand(
    {setFeatureCompatibilityVersion: latestFCV, writeConcern: {wtimeout: 1000}});
assert.eq(0, res.ok);
assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
restartServerReplication(secondary);

// Because the failed setFCV command left the primary in an intermediary state, complete the
// upgrade then reset back to the lastLTS version.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

secondary = rst.add({binVersion: lastLTS});
secondaryAdminDB = secondary.getDB("admin");

// Rig the election so that the first node running latest version remains the primary after the
// 'lastLTS' secondary is added to the replica set.
replSetConfig = rst.getReplSetConfig();
replSetConfig.version = 4;
replSetConfig.members[2].priority = 0;
reconfig(rst, replSetConfig);

// Verify that the 'lastLTS' secondary successfully performed its initial sync.
assert.commandWorked(
    primaryAdminDB.getSiblingDB("test").coll.insert({awaitRepl: true}, {writeConcern: {w: 3}}));

// Test that a 'lastLTS' secondary can no longer replicate from the primary after the FCV is
// upgraded to 'latestFCV'.
// Note: the 'lastLTS' secondary must stop replicating during the upgrade to ensure it has no
// chance of seeing the 'upgrading to latest' message in the oplog, whereupon it would crash.
stopServerReplication(secondary);
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
restartServerReplication(secondary);
checkFCV(secondaryAdminDB, lastLTSFCV);
assert.commandWorked(primaryAdminDB.getSiblingDB("test").coll.insert({shouldReplicate: false}));
assert.eq(secondaryAdminDB.getSiblingDB("test").coll.find({shouldReplicate: false}).itcount(), 0);
rst.stopSet();

// Test idempotency for setFeatureCompatibilityVersion.
rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
rst.startSet();
rst.initiate();

// Set FCV to 'lastLTSFCV' so that a 'lastLTS' binary node can join the set.
primary = rst.getPrimary();
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
rst.awaitReplication();

// Add a 'lastLTS' binary node to the set.
secondary = rst.add({binVersion: lastLTS});
rst.reInitiate();

// Ensure the 'lastLTS' binary node succeeded its initial sync.
assert.commandWorked(primary.getDB("test").coll.insert({awaitRepl: true}, {writeConcern: {w: 3}}));

// Run {setFCV: lastLTSFCV}. This should be idempotent.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
rst.awaitReplication();

// Ensure the secondary is still running.
rst.stopSet();

//
// Sharding tests.
//

let st;
let mongosAdminDB;
let configPrimaryAdminDB;
let shardPrimaryAdminDB;

// A 'latest' binary cluster started with clean data files will set FCV to 'latestFCV'.
st = new ShardingTest({
    shards: {rs0: {nodes: [{binVersion: latest}, {binVersion: latest}]}},
    other: {useBridge: true}
});
mongosAdminDB = st.s.getDB("admin");
configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");

checkFCV(configPrimaryAdminDB, latestFCV);
checkFCV(shardPrimaryAdminDB, latestFCV);

jsTestLog("EXPECTED TO FAIL: featureCompatibilityVersion cannot be set to invalid value on mongos");
assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: 5}));
assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: "4.8"}));

jsTestLog("EXPECTED TO FAIL: setFeatureCompatibilityVersion rejects unknown fields on mongos");
assert.commandFailed(
    mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, unknown: 1}));

jsTestLog(
    "EXPECTED TO FAIL: setFeatureCompatibilityVersion can only be run on the admin database on mongos");
assert.commandFailed(st.s.getDB("test").runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

jsTestLog("EXPECTED TO FAIL: featureCompatibilityVersion cannot be set via setParameter on mongos");
assert.commandFailed(
    mongosAdminDB.runCommand({setParameter: 1, featureCompatibilityVersion: lastLTSFCV}));

// Prevent the shard primary from receiving messages from the config server primary. When we try
// to set FCV to 'lastLTSFCV', the command should fail because the shard cannot be contacted.
st.rs0.getPrimary().discardMessagesFrom(st.configRS.getPrimary(), 1.0);
jsTestLog(
    "EXPECTED TO FAIL: setFeatureCompatibilityVersion cannot be set because the shard primary is not reachable");
assert.commandFailed(
    mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, maxTimeMS: 1000}));
checkFCV(configPrimaryAdminDB, lastLTSFCV, lastLTSFCV /* indicates downgrade in progress */);
st.rs0.getPrimary().discardMessagesFrom(st.configRS.getPrimary(), 0.0);

// FCV can be set to 'lastLTSFCV' on mongos.
// This is run through assert.soon() because we've just caused a network interruption
// by discarding messages in the bridge.
assert.soon(function() {
    res = mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV});
    if (res.ok == 0) {
        print("Failed to set feature compatibility version: " + tojson(res));
        return false;
    }
    return true;
});

// featureCompatibilityVersion propagates to config and shard.
checkFCV(configPrimaryAdminDB, lastLTSFCV);
checkFCV(shardPrimaryAdminDB, lastLTSFCV);

// A 'latest' binary replica set started as a shard server defaults to 'lastLTSFCV'.
let latestShard = new ReplSetTest({
    name: "latestShard",
    nodes: [{binVersion: latest}, {binVersion: latest}],
    nodeOptions: {shardsvr: ""},
    useHostName: true
});
latestShard.startSet();
latestShard.initiate();
let latestShardPrimaryAdminDB = latestShard.getPrimary().getDB("admin");
checkFCV(latestShardPrimaryAdminDB, lastLTSFCV);
assert.commandWorked(mongosAdminDB.runCommand({addShard: latestShard.getURL()}));
checkFCV(latestShardPrimaryAdminDB, lastLTSFCV);

// FCV can be set to 'latestFCV' on mongos.
assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);
checkFCV(shardPrimaryAdminDB, latestFCV);
checkFCV(latestShardPrimaryAdminDB, latestFCV);

// Call ShardingTest.stop before shutting down latestShard, so that the UUID check in
// ShardingTest.stop can talk to latestShard.
st.stop();
latestShard.stopSet();

// Create cluster with a 'lastLTS' binary mongos so that we can add 'lastLTS' binary
// shards.
st = new ShardingTest({shards: 0, other: {mongosOptions: {binVersion: lastLTS}}});
mongosAdminDB = st.s.getDB("admin");
configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
checkFCV(configPrimaryAdminDB, lastLTSFCV);

// Adding a 'lastLTS' binary shard to a cluster with 'lastLTSFCV' succeeds.
let lastLTSShard = new ReplSetTest({
    name: "lastLTSShard",
    nodes: [{binVersion: lastLTS}, {binVersion: lastLTS}],
    nodeOptions: {shardsvr: ""},
    useHostName: true
});
lastLTSShard.startSet();
lastLTSShard.initiate();
assert.commandWorked(mongosAdminDB.runCommand({addShard: lastLTSShard.getURL()}));
checkFCV(lastLTSShard.getPrimary().getDB("admin"), lastLTSFCV);

// call ShardingTest.stop before shutting down lastLTSShard, so that the UUID check in
// ShardingTest.stop can talk to lastLTSShard.
st.stop();
lastLTSShard.stopSet();
})();
