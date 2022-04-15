/*
 * Test that the in-memory execution history of write statements executed in a retryable internal
 * transaction gets discarded after the transaction aborts.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});

const kDbName = "testDb";
const kCollName = "testColl";
let testDB = st.rs0.getPrimary().getDB(kDbName);
let testColl = testDB.getCollection(kCollName);

assert.commandWorked(testDB.createCollection(kCollName));

function makeSessionOpts() {
    const sessionUUID = UUID();
    const parentLsid = {id: sessionUUID};
    let parentTxnNumber = NumberLong(35);
    const childLsid = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
    const childTxnNumber = NumberLong(0);
    let stmtId = 0;
    return {parentLsid, parentTxnNumber, childLsid, childTxnNumber, stmtId};
}

function makeInsertCmdObjForRetryableWrite(lsid, txnNumber, stmtId, doc) {
    return {
        insert: kCollName,
        documents: [doc],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
    };
}

function makeInsertCmdObjForTransaction(lsid, txnNumber, stmtId, doc) {
    return {
        insert: kCollName,
        documents: [doc],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        autocommit: false
    };
}

{
    function runTest({isPreparedTxn}) {
        jsTest.log(
            `Test aborting ${isPreparedTxn ? "a prepared" : "an unprepared"} retryable internal ` +
            "transaction and retrying the write statements executed in the aborted transaction " +
            "and in a retryable write prior to the transaction");
        let {parentLsid, parentTxnNumber, childLsid, childTxnNumber, stmtId} = makeSessionOpts();

        // Execute a write statement in a retryable write.
        const parentSessionCmdObj =
            makeInsertCmdObjForRetryableWrite(parentLsid, parentTxnNumber, stmtId++, {_id: 0});
        assert.commandWorked(testDB.runCommand(parentSessionCmdObj));

        // Execute another write statement in a retryable internal transaction.
        const childSessionCmdObj1 = Object.assign(
            makeInsertCmdObjForTransaction(childLsid, childTxnNumber, stmtId++, {_id: 1}),
            {startTransaction: true});
        assert.commandWorked(testDB.runCommand(childSessionCmdObj1));

        // Prepare and abort the transaction.
        if (isPreparedTxn) {
            const prepareCmdObj = makePrepareTransactionCmdObj(childLsid, childTxnNumber);
            assert.commandWorked(testDB.adminCommand(prepareCmdObj));
        }
        const abortCmdObj = makeAbortTransactionCmdObj(childLsid, childTxnNumber);
        assert.commandWorked(testDB.adminCommand(abortCmdObj));

        // Retry all write statements including the one executed as a retryable write and verify
        // that they execute exactly once.
        const retryResForParentSessionCmdObj =
            assert.commandWorked(testDB.runCommand(parentSessionCmdObj));
        assert.eq(retryResForParentSessionCmdObj.n, 1);

        assert.eq(testColl.find({_id: 0}).itcount(), 1);
        assert.eq(testColl.find({_id: 1}).itcount(), 0);

        // If the in-memory execution history did not get discarded after the transaction aborted,
        // the write statements in the commands below would not execute, and therefore the doc
        // {_id: 1} would not exist.
        const parentSessionCmdObj1 = makeInsertCmdObjForRetryableWrite(
            parentLsid, parentTxnNumber, childSessionCmdObj1.stmtId, {_id: 1});
        const retryResForParentSessionCmdObj1 =
            assert.commandWorked(testDB.runCommand(parentSessionCmdObj1));
        assert.eq(retryResForParentSessionCmdObj1.n, 1);

        assert.eq(testColl.find({_id: 0}).itcount(), 1);
        assert.eq(testColl.find({_id: 1}).itcount(), 1);

        assert.commandWorked(testColl.remove({}));
    }

    runTest({isPreparedTxn: true});
    runTest({isPreparedTxn: false});
}

{
    function runTest({isPreparedTxn}) {
        jsTest.log(
            `Test aborting ${isPreparedTxn ? "a prepared" : "an unprepared"} retryable ` +
            "internal transaction and running the transaction with different write statements");
        let {parentLsid, parentTxnNumber, childLsid, childTxnNumber, stmtId} = makeSessionOpts();

        // Execute a write statement in a retryable write.
        const parentSessionCmdObj =
            makeInsertCmdObjForRetryableWrite(parentLsid, parentTxnNumber, stmtId++, {_id: 0});
        assert.commandWorked(testDB.runCommand(parentSessionCmdObj));

        // Execute another write statement in a retryable internal transaction.
        const childSessionCmdObj1 = Object.assign(
            makeInsertCmdObjForTransaction(childLsid, childTxnNumber, stmtId++, {_id: 1}),
            {startTransaction: true});
        assert.commandWorked(testDB.runCommand(childSessionCmdObj1));

        // Prepare and abort the transaction.
        if (isPreparedTxn) {
            const prepareCmdObj = makePrepareTransactionCmdObj(childLsid, childTxnNumber);
            assert.commandWorked(testDB.adminCommand(prepareCmdObj));
        }
        const abortCmdObj = makeAbortTransactionCmdObj(childLsid, childTxnNumber);
        assert.commandWorked(testDB.adminCommand(abortCmdObj));

        // Retry.
        const retryResForParentSessionCmdObj =
            assert.commandWorked(testDB.runCommand(parentSessionCmdObj));
        assert.eq(retryResForParentSessionCmdObj.n, 1);

        assert.eq(testColl.find({_id: 0}).itcount(), 1);
        assert.eq(testColl.find({_id: 1}).itcount(), 0);

        const childSessionCmdObj2 = Object.assign(
            makeInsertCmdObjForTransaction(childLsid, childTxnNumber, stmtId++, {_id: 2}),
            {startTransaction: true});

        if (isPreparedTxn) {
            // It is illegal to retry an aborted prepared transaction.
            assert.commandFailedWithCode(testDB.runCommand(childSessionCmdObj2), 50911);

            assert.eq(testColl.find({_id: 0}).itcount(), 1);
            assert.eq(testColl.find({_id: 1}).itcount(), 0);
            assert.eq(testColl.find({_id: 2}).itcount(), 0);
        } else {
            // If the in-memory execution history did not get discarded after the transaction
            // aborted, the write statement to insert the doc {_id: `} would get committed in this
            // retry transaction.
            assert.commandWorked(testDB.runCommand(childSessionCmdObj2));
            const commitCmdObj = makeCommitTransactionCmdObj(childLsid, childTxnNumber);
            assert.commandWorked(testDB.adminCommand(commitCmdObj));

            assert.eq(testColl.find({_id: 0}).itcount(), 1);
            assert.eq(testColl.find({_id: 1}).itcount(), 0);
            assert.eq(testColl.find({_id: 2}).itcount(), 1);
        }

        assert.commandWorked(testColl.remove({}));
    }

    runTest({isPreparedTxn: true});
    runTest({isPreparedTxn: false});
}

st.stop();
})();
