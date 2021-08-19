/*
 * Tests that the client can only retry a transaction that failed with a transient transaction error
 * by attaching a higher txnRetryCounter when the FCV is latest.
 *
 * @tags: [requires_fcv_51, featureFlagInternalTransactions]
 */
(function() {
'use strict';

function runTest(downgradeFCV) {
    load("jstests/libs/fail_point_util.js");

    const st = new ShardingTest({shards: 1, rs: {nodes: 3}});
    const shard0Rst = st.rs0;
    const shard0Primary = shard0Rst.getPrimary();

    const kDbName = "testDb";
    const kCollName = "testColl";
    const kNs = kDbName + "." + kCollName;
    const testDB = shard0Primary.getDB(kDbName);

    jsTest.log("Verify that txnRetryCounter is only supported in FCV latest");

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    const sessionUUID = UUID();
    const lsid = {id: sessionUUID};
    const txnNumber = NumberLong(1);
    configureFailPoint(shard0Primary,
                       "failCommand",
                       {
                           failInternalCommands: true,
                           failCommands: ["insert"],
                           errorCode: ErrorCodes.LockBusy,
                           namespace: kNs
                       },
                       {times: 1});
    const insertCmdObj = {
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false,
        txnRetryCounter: NumberInt(0)
    };
    assert.commandFailedWithCode(testDB.runCommand(insertCmdObj), ErrorCodes.InvalidOptions);

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    assert.commandFailedWithCode(testDB.runCommand(insertCmdObj), ErrorCodes.LockBusy);
    insertCmdObj.txnRetryCounter = NumberInt(1);
    assert.commandWorked(testDB.runCommand(insertCmdObj));
    assert.commandWorked(testDB.adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
        txnRetryCounter: insertCmdObj.txnRetryCounter
    }));

    st.stop();
}

runFeatureFlagMultiversionTest('featureFlagInternalTransactions', runTest);
})();
