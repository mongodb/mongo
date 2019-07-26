/**
 * Tests upgrading a cluster from last stable to current version, verifying the behavior of
 * transactions and retryable writes throughout the process.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

// Checking UUID consistency uses cached connections, which are not valid across restarts or
// stepdowns.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";

load("jstests/libs/feature_compatibility_version.js");
load("jstests/multiVersion/libs/multi_rs.js");
load("jstests/multiVersion/libs/multi_cluster.js");
load("jstests/multiVersion/libs/sharded_txn_upgrade_downgrade_cluster_shared.js");

const dbName = "test";
const collName = "sharded_txn_upgrade_cluster";

// Start a cluster with two shards at the last stable version.
const st = setUpTwoShardClusterWithBinVersion(dbName, collName, "last-stable");

const txnIds = {
    commit: {lsid: {id: UUID()}, txnNumber: 0},
    commitMulti: {lsid: {id: UUID()}, txnNumber: 0},
    write: {lsid: {id: UUID()}, txnNumber: 0},
};

let testDB = st.s.getDB(dbName);

// Only retryable writes work and they are retryable.
assertMultiShardRetryableWriteWorked(testDB, collName, txnIds.write);
assertMultiShardRetryableWriteCanBeRetried(testDB, collName, txnIds.write);

// Upgrade the config servers.
jsTestLog("Upgrading config servers.");
st.upgradeCluster("latest", {upgradeConfigs: true, upgradeMongos: false, upgradeShards: false});

// Then upgrade the shard servers.
jsTestLog("Upgrading shard servers.");
st.upgradeCluster("latest", {upgradeConfigs: false, upgradeMongos: false, upgradeShards: true});

// Then upgrade mongos servers.
jsTestLog("Upgrading mongos servers.");
st.upgradeCluster("latest", {upgradeConfigs: false, upgradeMongos: true, upgradeShards: false});
checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);

testDB = st.s.getDB(dbName);

// Can still retry the retryable write.
assertMultiShardRetryableWriteCanBeRetried(testDB, collName, txnIds.write);

// Transactions that don't use prepare are allowed in FCV 4.0 with a 4.2 binary mongos.
assert.commandWorked(runTxn(testDB, collName, txnIds.commit, {multiShard: false}));

// Multi shard transactions will fail because coordinateCommit is not allowed in FCV 4.0.
assert.commandFailedWithCode(runTxn(testDB, collName, txnIds.commitMulti, {multiShard: true}),
                             ErrorCodes.CommandNotSupported);

// Upgrade the cluster's feature compatibility version to the latest.
assert.commandWorked(st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);

// Can still retry the retryable write and the committed transaction.
assertMultiShardRetryableWriteCanBeRetried(testDB, collName, txnIds.write);
assert.commandWorked(retryCommit(testDB, txnIds.commit));

// Can perform a new operation on each session.
Object.keys(txnIds).forEach((txnIdKey) => {
    txnIds[txnIdKey].txnNumber += 1;
});

assert.commandWorked(runTxn(testDB, collName, txnIds.commit, {multiShard: false}));
assert.commandWorked(runTxn(testDB, collName, txnIds.commitMulti, {multiShard: true}));
assertMultiShardRetryableWriteWorked(testDB, collName, txnIds.write);

st.stop();
})();
