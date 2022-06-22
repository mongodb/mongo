/*
 * Tests the setFCV command creates/removes the partial config.transactions index for retryable
 * transactions on upgrade/downgrade.
 *
 * @tags: [uses_transactions]
 */
(function() {
"use strict";

// This test does direct writes to config.transactions which requires not using a session.
TestData.disableImplicitSessions = true;

load("jstests/replsets/rslib.js");

// Verifies both the _id index and partial parent_lsid index exists for config.transactions.
function assertPartialIndexExists(node) {
    const configDB = node.getDB("config");
    const indexSpecs = assert.commandWorked(configDB.runCommand({"listIndexes": "transactions"}))
                           .cursor.firstBatch;
    indexSpecs.sort((index0, index1) => index0.name > index1.name);
    assert.eq(indexSpecs.length, 2);
    const idIndexSpec = indexSpecs[0];
    assert.eq(idIndexSpec.key, {"_id": 1});
    const partialIndexSpec = indexSpecs[1];
    assert.eq(partialIndexSpec.name, "parent_lsid");
    assert.eq(partialIndexSpec.key, {"parentLsid": 1, "_id.txnNumber": 1, "_id": 1});
    assert.eq(partialIndexSpec.partialFilterExpression, {"parentLsid": {"$exists": true}});
}

// Verifies only the _id index exists for config.transactions.
function assertPartialIndexDoesNotExist(node) {
    const configDB = node.getDB("config");
    const indexSpecs = assert.commandWorked(configDB.runCommand({"listIndexes": "transactions"}))
                           .cursor.firstBatch;
    assert.eq(indexSpecs.length, 1);
    const idIndexSpec = indexSpecs[0];
    assert.eq(idIndexSpec.key, {"_id": 1});
}

/**
 * Verifies the partial index is dropped/created on FCV transitions and retryable writes work in all
 * FCVs.
 */
function runTest(
    setFCVConn, modifyIndexConns, verifyIndexConns, rst, alwaysCreateFeatureFlagEnabled) {
    // Start at latest FCV which should have the index.
    assert.commandWorked(setFCVConn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    verifyIndexConns.forEach(conn => {
        assertPartialIndexExists(conn);
    });

    // Downgrade to last LTS removes index.
    assert.commandWorked(setFCVConn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    verifyIndexConns.forEach(conn => {
        assertPartialIndexDoesNotExist(conn);
    });

    assert.commandWorked(setFCVConn.getDB("foo").runCommand(
        {insert: "bar", documents: [{x: 1}], lsid: {id: UUID()}, txnNumber: NumberLong(11)}));

    if (rst) {
        // On step up to primary the index should not be created. Note this tests the empty
        // collection case when alwaysCreateFeatureFlagEnabled is true.

        let primary = rst.getPrimary();
        // Clear the collection so we'd try to create the index.
        assert.commandWorked(primary.getDB("config").transactions.remove({}));
        assert.commandWorked(
            primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));

        setFCVConn = rst.getPrimary();
        modifyIndexConns = [rst.getPrimary()];
        rst.awaitReplication();
        verifyIndexConns.forEach(conn => {
            reconnect(conn);
            if (alwaysCreateFeatureFlagEnabled) {
                assertPartialIndexExists(conn);
            } else {
                assertPartialIndexDoesNotExist(conn);
            }
        });

        if (alwaysCreateFeatureFlagEnabled) {
            // The test expects no index after this block, so remove it.
            modifyIndexConns.forEach(conn => {
                assert.commandWorked(
                    conn.getCollection("config.transactions").dropIndex("parent_lsid"));
            });
        }
        rst.awaitReplication();
    }

    assert.commandWorked(setFCVConn.getDB("foo").runCommand(
        {insert: "bar", documents: [{x: 1}], lsid: {id: UUID()}, txnNumber: NumberLong(11)}));

    // Upgrade from last LTS to latest adds index.
    assert.commandWorked(setFCVConn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    verifyIndexConns.forEach(conn => {
        assertPartialIndexExists(conn);
    });

    assert.commandWorked(setFCVConn.getDB("foo").runCommand(
        {insert: "bar", documents: [{x: 1}], lsid: {id: UUID()}, txnNumber: NumberLong(11)}));

    // Downgrade from latest to last continuous removes index.
    assert.commandWorked(
        setFCVConn.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));
    verifyIndexConns.forEach(conn => {
        assertPartialIndexDoesNotExist(conn);
    });

    if (rst) {
        // On step up to primary the index should not be created. Note this tests the non-empty
        // collection case when alwaysCreateFeatureFlagEnabled is true.

        let primary = rst.getPrimary();
        // Clear the collection so we'd try to create the index. Skip if the always create index
        // feature flag is on because we'd try to create the index anyway.
        if (!alwaysCreateFeatureFlagEnabled) {
            assert.commandWorked(primary.getDB("config").transactions.remove({}));
        }
        assert.commandWorked(
            primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));

        setFCVConn = rst.getPrimary();
        modifyIndexConns = [rst.getPrimary()];
        rst.awaitReplication();
        verifyIndexConns.forEach(conn => {
            reconnect(conn);
            if (alwaysCreateFeatureFlagEnabled) {
                assertPartialIndexExists(conn);
            } else {
                assertPartialIndexDoesNotExist(conn);
            }
        });

        if (alwaysCreateFeatureFlagEnabled) {
            // The test expects no index after this block, so remove it.
            modifyIndexConns.forEach(conn => {
                assert.commandWorked(
                    conn.getCollection("config.transactions").dropIndex("parent_lsid"));
            });
        }
        rst.awaitReplication();
    }

    assert.commandWorked(setFCVConn.getDB("foo").runCommand(
        {insert: "bar", documents: [{x: 1}], lsid: {id: UUID()}, txnNumber: NumberLong(11)}));

    // Upgrade from last continuous to latest LTS adds index.
    assert.commandWorked(setFCVConn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    verifyIndexConns.forEach(conn => {
        assertPartialIndexExists(conn);
    });

    if (rst) {
        // On step up to primary the index should be created.

        let primary = rst.getPrimary();
        // Clear the collection so we'd try to create the index. Skip if the always create index
        // feature flag is on because we'd try to create the index anyway.
        if (!alwaysCreateFeatureFlagEnabled) {
            assert.commandWorked(primary.getDB("config").transactions.remove({}));
        }
        assert.commandWorked(
            primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));

        setFCVConn = rst.getPrimary();
        modifyIndexConns = [rst.getPrimary()];
        rst.awaitReplication();
        verifyIndexConns.forEach(conn => {
            reconnect(conn);
            assertPartialIndexExists(conn);
        });
    }

    assert.commandWorked(setFCVConn.getDB("foo").runCommand(
        {insert: "bar", documents: [{x: 1}], lsid: {id: UUID()}, txnNumber: NumberLong(11)}));

    // Verify downgrading ignores IndexNotFound.
    modifyIndexConns.forEach(conn => {
        assert.commandWorked(conn.getCollection("config.transactions").dropIndex("parent_lsid"));
    });
    assert.commandWorked(setFCVConn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    verifyIndexConns.forEach(conn => {
        assertPartialIndexDoesNotExist(conn);
    });

    // Verify upgrading works if the index already exists.
    modifyIndexConns.forEach(conn => {
        assert.commandWorked(conn.getDB("config").runCommand({
            createIndexes: "transactions",
            indexes: [{
                v: 2,
                partialFilterExpression: {parentLsid: {$exists: true}},
                name: "parent_lsid",
                key: {parentLsid: 1, "_id.txnNumber": 1, _id: 1}
            }],
        }));
    });
    assert.commandWorked(setFCVConn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    verifyIndexConns.forEach(conn => {
        assertPartialIndexExists(conn);
    });
}

{
    const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
    // Note setFCV always waits for majority write concern so in a two node cluster secondaries will
    // always have replicated the setFCV writes.
    runTest(st.s, [st.rs0.getPrimary(), st.configRS.getPrimary()], [
        st.rs0.getPrimary(),
        st.rs0.getSecondary(),
        st.configRS.getPrimary(),
        st.configRS.getSecondary()
    ]);
    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    // Note setFCV always waits for majority write concern so in a two node cluster secondaries will
    // always have replicated the setFCV writes.
    runTest(rst.getPrimary(), [rst.getPrimary()], [rst.getPrimary(), rst.getSecondary()], rst);
    rst.stopSet();
}

{
    // Enabling featureFlagAlwaysCreateConfigTransactionsPartialIndexOnStepUp should not lead to
    // creating the index if the internal transactions feature flag is not enabled.
    const featureFlagRst = new ReplSetTest({
        nodes: 2,
        nodeOptions:
            {setParameter: "featureFlagAlwaysCreateConfigTransactionsPartialIndexOnStepUp=true"}
    });
    featureFlagRst.startSet();
    featureFlagRst.initiate();
    // Note setFCV always waits for majority write concern so in a two node cluster secondaries will
    // always have replicated the setFCV writes.
    runTest(featureFlagRst.getPrimary(),
            [featureFlagRst.getPrimary()],
            [featureFlagRst.getPrimary(), featureFlagRst.getSecondary()],
            featureFlagRst,
            true /* alwaysCreateFeatureFlagEnabled */);
    featureFlagRst.stopSet();
}

{
    const conn = MongoRunner.runMongod();

    const configTxnsCollection = conn.getCollection("config.transactions");
    assert(!configTxnsCollection.exists());

    // Verify each upgrade/downgrade path can succeed and won't implicitly create
    // config.transactions, which doesn't exist on standalone mongods.
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    assert(!configTxnsCollection.exists());

    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    assert(!configTxnsCollection.exists());

    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));
    assert(!configTxnsCollection.exists());

    MongoRunner.stopMongod(conn);
}
})();
