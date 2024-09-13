/* Verifies behavior on replica sets and sharded clusters with inconsistent config.transactions
 * formats (clustered and non-clustered).
 * TODO (SERVER-84073) eventually we should replace uses of setParameter with binary versions
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
// Users are prohibited from dropping config.transactions while any sessions are open;
// disable implicit sessions so that we can drop config.transactions down below.
TestData.disableImplicitSessions = true;

import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const storageEngine = jsTest.options().storageEngine || 'wiredTiger';

// Verifies that config.transactions is of the format specified in options on all nodes. Only valid
// values are {clustered: true} and {clustered: false}.
function verifyConfigTxnsFormat(replSet, options) {
    const clusteredOptions = ClusteredCollectionUtil.constructFullCreateOptions(
        {clusteredIndex: {key: {_id: 1}, unique: true}});
    for (const node of replSet.nodes) {
        const configTxns = node.getDB('config');
        if (options.clustered)
            ClusteredCollectionUtil.validateListCollections(
                configTxns, 'transactions', clusteredOptions);
        else
            ClusteredCollectionUtil.validateListCollectionsNotClustered(configTxns, 'transactions');
    }
}

// Opens transactions numbered [startIdx, endIdx) and closes them out. Returns endIdx + 1.
function openAndCloseTransactions(primaryOrRouter, startIdx, endIdx, transactionCB) {
    const dbName = 'testDB';
    const collName = 'testColl';
    startIdx = startIdx || 0;
    endIdx = endIdx || (startIdx + 20);
    transactionCB = transactionCB || function(sessionColl, txnNum) {
        assert.commandWorked(sessionColl.insert({_id: "txn-" + txnNum}));
    };

    const coll = primaryOrRouter.getDB(dbName)[collName];
    assert.commandWorked(
        coll.insert({_id: 'pretransaction' + startIdx, x: 0}, {writeConcern: {w: "majority"}}));
    for (let txnNum = startIdx; txnNum < endIdx; txnNum++) {
        const session = primaryOrRouter.startSession({causalConsistency: false});
        session.startTransaction();
        const sessionColl = session.getDatabase(dbName)[collName];
        transactionCB(sessionColl, txnNum);
        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();
    }
    return endIdx + 1;
}

// Verifies that the secondaries follow the format in which config.transactions was created on the
// primary. Additionally verifies that accessing config.transactions as a clustered collection
// works.
function testSecondaryReplicationOfConfigTxnsFormat() {
    // Start the primary with the clustered config.transaction feature flag set so that it creates
    // the config.transactions collection with a clustered index.
    const replSet = new ReplSetTest({
        name: jsTestName(),
        nodes: [
            {setParameter: {featureFlagClusteredConfigTransactions: true}},
            {setParameter: {featureFlagClusteredConfigTransactions: false}}
        ]
    });
    replSet.startSet();
    replSet.initiate();
    verifyConfigTxnsFormat(replSet, {clustered: true});

    // Open some transactions and then close them out to verify that accessing config.transactions
    // as a clustered collection works.
    let nextTxnNum = openAndCloseTransactions(replSet.getPrimary());

    // Drop config.transactions and change primary to force re-creation as a nonclustered
    // collection.
    const primary = replSet.getPrimary();
    const secondary = replSet.getSecondary();
    const primaryConfig = primary.getDB('config');
    assert.commandWorked(primaryConfig.runCommand({drop: 'transactions'}));
    replSet.awaitReplication();
    assert.commandWorked(secondary.adminCommand({replSetStepUp: 1}));
    replSet.awaitNodesAgreeOnPrimary();
    assert.eq(secondary, replSet.getPrimary());
    replSet.awaitReplication();
    verifyConfigTxnsFormat(replSet, {clustered: false});
    openAndCloseTransactions(replSet.getPrimary(), nextTxnNum);

    replSet.stopSet();
}

// Verify that restarting a secondary expecting the opposite config.transactions format from the
// primary works.
function testRestartSecondaryWithDifferentExpectedConfigTxnsFormat() {
    for (const primarySetting of [true, false]) {
        const replSet = new ReplSetTest({
            name: jsTestName(),
            nodes: [
                {setParameter: {featureFlagClusteredConfigTransactions: primarySetting}},
                {setParameter: {featureFlagClusteredConfigTransactions: primarySetting}}
            ]
        });
        replSet.startSet();
        replSet.initiate();

        verifyConfigTxnsFormat(replSet, {clustered: primarySetting});
        let nextTxnNum = openAndCloseTransactions(replSet.getPrimary());

        replSet.restart(1, {
            startClean: false,
            setParameter: {featureFlagClusteredConfigTransactions: !primarySetting}
        });
        replSet.awaitSecondaryNodes();
        replSet.reInitiate();
        replSet.awaitSecondaryNodes();

        // There's no step up, so config.transactions should still be the same format.
        verifyConfigTxnsFormat(replSet, {clustered: primarySetting});
        openAndCloseTransactions(replSet.getPrimary(), nextTxnNum);

        replSet.stopSet();
    }
}

// Verify that initial syncing from a primary while expecting the opposite config.transactions
// format from the primary works.
function testInitialSyncFromPrimaryWithDifferentExpectedConfigTxnsFormat(params) {
    for (const primarySetting of [true, false]) {
        params = params || {};
        params.featureFlagClusteredConfigTransactions = primarySetting;
        const replSet = new ReplSetTest({name: jsTestName(), nodes: [{setParameter: params}]});
        replSet.startSet();
        replSet.initiate();

        verifyConfigTxnsFormat(replSet, {clustered: primarySetting});
        let nextTxnNum = openAndCloseTransactions(replSet.getPrimary());

        params.featureFlagClusteredConfigTransactions = !primarySetting;
        const secondary = replSet.add({setParameter: params});
        replSet.reInitiate();
        replSet.awaitSecondaryNodes();

        // There's no step up, so config.transactions should still be the same format.
        verifyConfigTxnsFormat(replSet, {clustered: primarySetting});
        openAndCloseTransactions(replSet.getPrimary(), nextTxnNum);

        replSet.stopSet();
    }
}

// Verifies that transactions touching shards with different config.transactions formats work.
function testShardsWithDifferentConfigTxnsFormats() {
    const shardedCluster = new ShardingTest({
        name: jsTestName(),
        mongos: 1,
        shards: {
            rs0: {nodes: 1, setParameter: {featureFlagClusteredConfigTransactions: true}},
            rs1: {nodes: 1, setParameter: {featureFlagClusteredConfigTransactions: false}}
        }
    });

    // Replica set nodes in sharded clusters create config.transactions before upgrading to latest
    // FCV, so the primary on shard 0 will create a nonclustered config.transactions even though the
    // feature flag is set. We need to drop its config.transactions and step it down and up again so
    // that it recreates config.transactions as a clustered collection.
    ((shardedCluster) => {
        const primary = shardedCluster.rs0.getPrimary();
        const primaryConfig = primary.getDB('config');
        assert.commandWorked(primaryConfig.runCommand({drop: 'transactions'}));
        shardedCluster.rs0.awaitReplication();
        assert.commandWorked(primary.adminCommand({replSetStepDown: 1, force: true}));
        shardedCluster.rs0.awaitReplication();
        assert.commandWorked(primary.adminCommand({replSetStepUp: 1}));
        shardedCluster.rs0.awaitReplication();
    })(shardedCluster);

    verifyConfigTxnsFormat(shardedCluster.rs0, {clustered: true});
    verifyConfigTxnsFormat(shardedCluster.rs1, {clustered: false});

    // Set up the sharded collection with two chunks:
    // shard0: (-inf, 0)
    // shard1: [0, +inf)
    const dbName = 'testDB';
    const collName = 'testColl';
    const ns = dbName + '.' + collName;
    assert.commandWorked(shardedCluster.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(
        shardedCluster.s.adminCommand({movePrimary: dbName, to: shardedCluster.shard0.shardName}));
    assert.commandWorked(shardedCluster.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(shardedCluster.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(shardedCluster.s.adminCommand(
        {moveChunk: ns, find: {_id: 0}, to: shardedCluster.shard1.shardName}));

    // Open transactions whose documents will hit both shards, and then close them out.
    openAndCloseTransactions(shardedCluster.s, 0, 20, function(sessionColl, txnNum) {
        assert.commandWorked(sessionColl.insert({_id: txnNum + 1}));
        assert.commandWorked(sessionColl.insert({_id: 0 - txnNum}));
    });

    shardedCluster.stop();
}

testSecondaryReplicationOfConfigTxnsFormat();
testRestartSecondaryWithDifferentExpectedConfigTxnsFormat();
testInitialSyncFromPrimaryWithDifferentExpectedConfigTxnsFormat();  // logical initial sync
// File copy based sync requires the wired tiger storage engine; skip if using something else.
if (storageEngine == 'wiredTiger') {
    testInitialSyncFromPrimaryWithDifferentExpectedConfigTxnsFormat(
        {initialSyncMethod: 'fileCopyBased'});
}
testShardsWithDifferentConfigTxnsFormats();
