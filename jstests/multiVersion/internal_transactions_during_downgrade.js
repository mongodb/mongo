/*
 * Tests the setFCV command kills internal sessions with unprepared transactions that start
 * immediately before the FCV change and abort those transactions.
 *
 * @tags: [uses_transactions]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

function runInternalTxn(primaryHost, isStandaloneRst) {
    load("jstests/sharding/libs/sharded_transactions_helpers.js");

    const primary = new Mongo(primaryHost);

    const kDbName = "testDb";
    const kCollName = "testColl";
    let testDB = primary.getDB(kDbName);

    const sessionUUID = UUID();
    const parentLsid = {id: sessionUUID};
    const parentTxnNumber = NumberLong(35);
    const childLsid = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
    const childTxnNumber = NumberLong(0);
    const stmtId = NumberInt(1);

    const findAndModifyCmdObj = {
        findAndModify: kCollName,
        query: {_id: 0, x: 0},
        update: {$inc: {x: 1}},
        upsert: true,
        lsid: childLsid,
        txnNumber: childTxnNumber,
        startTransaction: true,
        autocommit: false,
        stmtId: stmtId,
    };
    const commitCmdObj = makeCommitTransactionCmdObj(childLsid, childTxnNumber);

    assert.commandFailedWithCode(testDB.runCommand(findAndModifyCmdObj), ErrorCodes.Interrupted);
    assert.commandFailedWithCode(testDB.adminCommand(commitCmdObj), ErrorCodes.NoSuchTransaction);
}

function runTest(rst, isStandaloneRst) {
    const primary = rst.getPrimary();
    let fp = configureFailPoint(primary, "hangAfterCheckingInternalTransactionsFeatureFlag");

    const internalTxnThread = new Thread(runInternalTxn, primary.host, isStandaloneRst);
    internalTxnThread.start();
    fp.wait();

    jsTest.log("Starting setFCV");
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    jsTest.log("Finished setFCV");

    fp.off();
    internalTxnThread.join();

    assert.eq(null, primary.getCollection("config.transactions").findOne());
}

{
    const st = new ShardingTest({shards: 1});
    runTest(st.rs0, false /* isStandaloneRst */);
    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    runTest(rst, true /* isStandaloneRst */);
    rst.stopSet();
}
})();
