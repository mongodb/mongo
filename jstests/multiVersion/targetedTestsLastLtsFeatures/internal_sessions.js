/*
 * Test that internal sessions are only supported in FCV latest.
 */
(function() {
'use strict';

function runTest(downgradeFCV) {
    TestData.disableImplicitSessions = true;

    const st = new ShardingTest({shards: 1});
    const shard0Rst = st.rs0;
    const shard0Primary = shard0Rst.getPrimary();

    const kDbName = "testDb";
    const kCollName = "testColl";
    const testDB = shard0Primary.getDB(kDbName);

    const sessionUUID = UUID();
    const lsid0 = {id: sessionUUID, txnNumber: NumberLong(35), txnUUID: UUID()};
    const txnNumber0 = NumberLong(0);
    const lsid1 = {id: sessionUUID, txnUUID: UUID()};
    const txnNumber1 = NumberLong(35);

    assert.commandWorked(shard0Primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    assert.commandFailedWithCode(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 0}],
        lsid: lsid0,
        txnNumber: txnNumber0,
        startTransaction: true,
        autocommit: false
    }),
                                 ErrorCodes.InternalTransactionNotSupported);
    assert.commandFailedWithCode(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid1,
        txnNumber: txnNumber1,
        startTransaction: true,
        autocommit: false
    }),
                                 ErrorCodes.InternalTransactionNotSupported);

    assert.commandWorked(shard0Primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 0}],
        lsid: lsid0,
        txnNumber: txnNumber0,
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: lsid0, txnNumber: txnNumber0, autocommit: false}));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid1,
        txnNumber: txnNumber1,
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(testDB.adminCommand(
        {commitTransaction: 1, lsid: lsid1, txnNumber: txnNumber1, autocommit: false}));

    st.stop();
}

runFeatureFlagMultiversionTest('featureFlagInternalTransactions', runTest);
})();
