/*
 * Tests that a client can retry a transaction that failed with a transient transaction error by
 * attaching a higher txnRetryCounter.
 *
 * @tags: [requires_fcv_51, featureFlagInternalTransactions]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const kDbName = "testDb";
const kCollName = "testColl";
const kNs = kDbName + "." + kCollName;

const st = new ShardingTest({shards: 1, rs: {nodes: 3}});
const shard0Rst = st.rs0;
const shard0Primary = shard0Rst.getPrimary();

const mongosTestDB = st.s.getDB(kDbName);
const shard0TestDB = shard0Primary.getDB(kDbName);
assert.commandWorked(mongosTestDB.createCollection(kCollName));

function testCommitAfterRetry(db, lsid, txnNumber) {
    const txnRetryCounter0 = NumberInt(0);
    const txnRetryCounter1 = NumberInt(1);

    jsTest.log(
        "Verify that the client can retry a transaction that failed with a transient " +
        "transaction error by attaching a higher txnRetryCounter and commit the transaction");
    configureFailPoint(shard0Primary,
                       "failCommand",
                       {
                           failInternalCommands: true,
                           failCommands: ["insert"],
                           errorCode: ErrorCodes.LockBusy,
                           namespace: kNs
                       },
                       {times: 1});
    const insertCmdObj0 = {
        insert: kCollName,
        documents: [{x: 0}],
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false,
    };
    assert.commandFailedWithCode(
        db.runCommand(Object.assign({}, insertCmdObj0, {txnRetryCounter: txnRetryCounter0})),
        ErrorCodes.LockBusy);
    assert.commandWorked(
        db.runCommand(Object.assign({}, insertCmdObj0, {txnRetryCounter: txnRetryCounter1})));

    jsTest.log("Verify that the client must attach the last used txnRetryCounter in all commands " +
               "in the transaction");
    const insertCmdObj1 = {
        insert: kCollName,
        documents: [{x: 1}],
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
    };
    const insertRes0 = assert.commandFailedWithCode(
        db.runCommand(Object.assign({}, insertCmdObj1, {txnRetryCounter: txnRetryCounter0})),
        ErrorCodes.TxnRetryCounterTooOld);
    assert.eq(txnRetryCounter1, insertRes0.txnRetryCounter, insertRes0);
    // txnRetryCounter defaults to 0.
    const insertRes1 = assert.commandFailedWithCode(db.runCommand(insertCmdObj1),
                                                    ErrorCodes.TxnRetryCounterTooOld);
    assert.eq(txnRetryCounter1, insertRes1.txnRetryCounter, insertRes1);
    assert.commandWorked(
        db.runCommand(Object.assign({}, insertCmdObj1, {txnRetryCounter: txnRetryCounter1})));

    jsTest.log("Verify that the client must attach the last used txnRetryCounter in the " +
               "commitTransaction command");
    const commitCmdObj = {
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
    };
    const commitRes = assert.commandFailedWithCode(
        db.adminCommand(Object.assign({}, commitCmdObj, {txnRetryCounter: txnRetryCounter0})),
        ErrorCodes.TxnRetryCounterTooOld);
    assert.eq(txnRetryCounter1, commitRes.txnRetryCounter, commitRes);

    assert.commandWorked(
        db.adminCommand(Object.assign({}, commitCmdObj, {txnRetryCounter: txnRetryCounter1})));
}

function testAbortAfterRetry(db, lsid, txnNumber) {
    const txnRetryCounter0 = NumberInt(0);
    const txnRetryCounter1 = NumberInt(1);

    jsTest.log("Verify that the client can retry a transaction that failed with a transient " +
               "transaction error by attaching a higher txnRetryCounter and abort the transaction");
    configureFailPoint(shard0Primary,
                       "failCommand",
                       {
                           failInternalCommands: true,
                           failCommands: ["insert"],
                           errorCode: ErrorCodes.LockBusy,
                           namespace: kNs
                       },
                       {times: 1});
    const insertCmdObj0 = {
        insert: kCollName,
        documents: [{x: 0}],
        lsid: lsid,
        txnNumber: txnNumber,
        startTransaction: true,
        autocommit: false,
    };
    assert.commandFailedWithCode(
        db.runCommand(Object.assign({}, insertCmdObj0, {txnRetryCounter: txnRetryCounter0})),
        ErrorCodes.LockBusy);
    assert.commandWorked(
        db.runCommand(Object.assign({}, insertCmdObj0, {txnRetryCounter: txnRetryCounter1})));

    jsTest.log("Verify that the client must attach the last used txnRetryCounter in the " +
               "abortTransaction command");
    const abortCmdObj = {
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
    };
    const abortRes = assert.commandFailedWithCode(
        db.adminCommand(Object.assign({}, abortCmdObj, {txnRetryCounter: txnRetryCounter0})),
        ErrorCodes.TxnRetryCounterTooOld);
    assert.eq(txnRetryCounter1, abortRes.txnRetryCounter, abortRes);

    assert.commandWorked(
        db.adminCommand(Object.assign({}, abortCmdObj, {txnRetryCounter: txnRetryCounter1})));
}

(() => {
    jsTest.log("Test transactions in a sharded cluster");
    const sessionUUID = UUID();
    const lsid0 = {id: sessionUUID};
    testCommitAfterRetry(mongosTestDB, lsid0, NumberLong(0));
    testAbortAfterRetry(mongosTestDB, lsid0, NumberLong(1));

    const lsid1 = {id: sessionUUID, txnNumber: NumberLong(1), stmtId: NumberInt(0)};
    testCommitAfterRetry(mongosTestDB, lsid1, NumberLong(0));
    testAbortAfterRetry(mongosTestDB, lsid1, NumberLong(1));

    const lsid2 = {id: sessionUUID, txnUUID: UUID()};
    testCommitAfterRetry(mongosTestDB, lsid2, NumberLong(0));
    testAbortAfterRetry(mongosTestDB, lsid2, NumberLong(1));
})();

(() => {
    jsTest.log("Test transactions in a replica set");
    const sessionUUID = UUID();
    const lsid0 = {id: sessionUUID};
    testCommitAfterRetry(shard0TestDB, lsid0, NumberLong(0));
    testAbortAfterRetry(shard0TestDB, lsid0, NumberLong(1));

    const lsid1 = {id: sessionUUID, txnNumber: NumberLong(1), stmtId: NumberInt(0)};
    testCommitAfterRetry(shard0TestDB, lsid1, NumberLong(0));
    testAbortAfterRetry(shard0TestDB, lsid1, NumberLong(1));

    const lsid2 = {id: sessionUUID, txnUUID: UUID()};
    testCommitAfterRetry(shard0TestDB, lsid2, NumberLong(0));
    testAbortAfterRetry(shard0TestDB, lsid2, NumberLong(1));
})();

st.stop();
})();
