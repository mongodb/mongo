/**
 * Tests downgrading a cluster from the current to last stable version succeeds, verifying the
 * behavior of transactions and retryable writes throughout the process.
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
    const collName = "sharded_txn_downgrade_cluster";

    // Start a cluster with two shards at the latest version.
    const st = setUpTwoShardClusterWithBinVersion(dbName, collName, "latest");

    const txnIds = {
        commit: {lsid: {id: UUID()}, txnNumber: 0},
        commitMulti: {lsid: {id: UUID()}, txnNumber: 0},
        write: {lsid: {id: UUID()}, txnNumber: 0},
    };

    let testDB = st.s.getDB(dbName);

    // Retryable writes and transactions with and without prepare should work.
    assert.commandWorked(runTxn(testDB, collName, txnIds.commit, {multiShard: false}));
    assert.commandWorked(runTxn(testDB, collName, txnIds.commitMulti, {multiShard: true}));
    assertMultiShardRetryableWriteWorked(testDB, collName, txnIds.write);

    // commitTransaction for both transactions and the retryable write should be retryable.
    assert.commandWorked(retryCommit(testDB, txnIds.commit));
    assert.commandWorked(retryCommit(testDB, txnIds.commitMulti));
    assertMultiShardRetryableWriteCanBeRetried(testDB, collName, txnIds.write);

    // Downgrade featureCompatibilityVersion.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);

    // Only the retryable write can be retried. Can't retry the multi shard transaction because it
    // uses coordinateCommit, which is not allowed in FCV 4.0.
    assertMultiShardRetryableWriteCanBeRetried(testDB, collName, txnIds.write);
    assert.commandFailedWithCode(retryCommit(testDB, txnIds.commit), ErrorCodes.NoSuchTransaction);
    assert.commandFailedWithCode(retryCommit(testDB, txnIds.commitMulti),
                                 ErrorCodes.CommandNotSupported);

    downgradeUniqueIndexesScript(st.s.getDB("test"));

    // Downgrade the mongos servers first.
    jsTestLog("Downgrading mongos servers.");
    st.upgradeCluster("last-stable",
                      {upgradeConfigs: false, upgradeMongos: true, upgradeShards: false});

    // Then downgrade the shard servers.
    jsTestLog("Downgrading shard servers.");
    st.upgradeCluster("last-stable",
                      {upgradeConfigs: false, upgradeMongos: false, upgradeShards: true});

    // Then downgrade the config servers.
    jsTestLog("Downgrading config servers.");
    st.upgradeCluster("last-stable",
                      {upgradeConfigs: true, upgradeMongos: false, upgradeShards: false});
    checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);

    testDB = st.s.getDB(dbName);

    // Can still retry the retryable write.
    assertMultiShardRetryableWriteCanBeRetried(testDB, collName, txnIds.write);

    // The txnIds used for the earlier commits should be re-usable because their history was
    // removed.
    assertMultiShardRetryableWriteWorked(testDB, collName, txnIds.commit);
    assertMultiShardRetryableWriteCanBeRetried(testDB, collName, txnIds.commit);

    assertMultiShardRetryableWriteWorked(testDB, collName, txnIds.commitMulti);
    assertMultiShardRetryableWriteCanBeRetried(testDB, collName, txnIds.commitMulti);

    // Can perform a new operation on each session.
    Object.keys(txnIds).forEach((txnIdKey) => {
        txnIds[txnIdKey].txnNumber += 1;
    });

    assertMultiShardRetryableWriteWorked(testDB, collName, txnIds.commit);
    assertMultiShardRetryableWriteWorked(testDB, collName, txnIds.commitMulti);
    assertMultiShardRetryableWriteWorked(testDB, collName, txnIds.write);

    st.stop();
})();
