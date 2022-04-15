/*
 * Test that prepared retryable internal transactions can commit and abort after failover and that
 * "cross-session" write statement execution works as expected after the transaction commits or
 * aborts.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});

const kDbName = "testDb";
const kCollName = "testColl";
const mongosTestDB = st.s.getDB(kDbName);
const mongosTestColl = mongosTestDB.getCollection(kCollName);
let shard0TestDB = st.rs0.getPrimary().getDB(kDbName);
let shard0TestColl = shard0TestDB.getCollection(kCollName);

assert.commandWorked(shard0TestDB.createCollection(kCollName));

// For creating documents that will result in large transactions.
const kSize10MB = 10 * 1024 * 1024;

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

function makeInsertCmdObjForTransaction(lsid, txnNumber, stmtId, doc, isLargeTxn) {
    return {
        insert: kCollName,
        documents: [Object.assign(doc, isLargeTxn ? {y: new Array(kSize10MB).join("a")} : {})],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        autocommit: false
    };
}

function makeInsertCmdObjForLargeTransaction(lsid, txnNumber, stmtId, doc) {
    return makeInsertCmdObjForTransaction(lsid, txnNumber, stmtId, doc, true);
}

function stepDownShard0Primary() {
    const oldPrimary = st.rs0.getPrimary();
    const oldSecondary = st.rs0.getSecondary();
    assert.commandWorked(oldSecondary.adminCommand({replSetFreeze: 0}));
    assert.commandWorked(
        oldPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    const newPrimary = st.rs0.getPrimary();
    assert.neq(oldPrimary, newPrimary);
    shard0TestDB = st.rs0.getPrimary().getDB(kDbName);
    shard0TestColl = shard0TestDB.getCollection(kCollName);
}

{
    jsTest.log("Test committing a prepared retryable internal transaction after failover and " +
               "retrying the write statements executed in the committed transaction and in a " +
               "retryable write prior to the transaction");
    // Upon failover, the new primary will not refresh the state for the transaction below from disk
    // since the transaction is in the prepared state. Therefore, retryability works if and only if
    // secondaries keep track of write statements executed in prepared retryable internal
    // transactions in-memory based on the transaction applyOps oplog entries.
    //
    // Test with both small and large transactions to provide test coverage for the case where the
    // transaction has multiple applyOps oplog entries.
    function runTest({isLargeTxn}) {
        let {parentLsid, parentTxnNumber, childLsid, childTxnNumber, stmtId} = makeSessionOpts();
        let makeInsertCmdObjForTransactionFunc =
            isLargeTxn ? makeInsertCmdObjForLargeTransaction : makeInsertCmdObjForTransaction;

        // Execute a write statement in a retryable write.
        const parentSessionCmdObj =
            makeInsertCmdObjForRetryableWrite(parentLsid, parentTxnNumber, stmtId++, {_id: 0});
        assert.commandWorked(shard0TestDB.runCommand(parentSessionCmdObj));

        // Execute additional write statements in a retryable internal transaction.
        const childSessionCmdObj1 = Object.assign(
            makeInsertCmdObjForTransactionFunc(childLsid, childTxnNumber, stmtId++, {_id: 1}),
            {startTransaction: true});
        assert.commandWorked(shard0TestDB.runCommand(childSessionCmdObj1));
        const childSessionCmdObj2 =
            makeInsertCmdObjForTransactionFunc(childLsid, childTxnNumber, stmtId++, {_id: 2});
        assert.commandWorked(shard0TestDB.runCommand(childSessionCmdObj2));
        const childSessionCmdObj3 =
            makeInsertCmdObjForTransactionFunc(childLsid, childTxnNumber, stmtId++, {_id: 3});
        assert.commandWorked(shard0TestDB.runCommand(childSessionCmdObj3));

        // Prepare the transaction.
        const prepareCmdObj = makePrepareTransactionCmdObj(childLsid, childTxnNumber);
        const preparedTxnRes = assert.commandWorked(shard0TestDB.adminCommand(prepareCmdObj));

        stepDownShard0Primary();

        // Commit the transaction after failover.
        const commitCmdObj = makeCommitTransactionCmdObj(childLsid, childTxnNumber);
        commitCmdObj.commitTimestamp = preparedTxnRes.prepareTimestamp;
        assert.commandWorked(shard0TestDB.adminCommand(commitCmdObj));

        // Retry all write statements including the one executed as a retryable write and verify
        // that they execute exactly once.
        assert.commandWorked(shard0TestDB.runCommand(parentSessionCmdObj));
        assert.commandWorked(shard0TestDB.runCommand(childSessionCmdObj1));
        assert.commandWorked(shard0TestDB.runCommand(childSessionCmdObj2));
        assert.commandWorked(shard0TestDB.runCommand(childSessionCmdObj3));
        assert.commandWorked(shard0TestDB.adminCommand(commitCmdObj));

        assert.eq(shard0TestColl.find({_id: 0}).itcount(), 1);
        assert.eq(shard0TestColl.find({_id: 1}).itcount(), 1);
        assert.eq(shard0TestColl.find({_id: 2}).itcount(), 1);
        assert.eq(shard0TestColl.find({_id: 3}).itcount(), 1);

        const parentSessionCmdObj1 = makeInsertCmdObjForRetryableWrite(
            parentLsid, parentTxnNumber, childSessionCmdObj1.stmtId, {_id: 1});
        const retryResForParentSessionCmdObj1 =
            assert.commandWorked(shard0TestDB.runCommand(parentSessionCmdObj1));
        assert.eq(retryResForParentSessionCmdObj1.n, 1);

        const parentSessionCmdObj2 = makeInsertCmdObjForRetryableWrite(
            parentLsid, parentTxnNumber, childSessionCmdObj2.stmtId, {_id: 2});
        const retryResForParentSessionCmdObj2 =
            assert.commandWorked(shard0TestDB.runCommand(parentSessionCmdObj2));
        assert.eq(retryResForParentSessionCmdObj2.n, 1);

        const parentSessionCmdObj3 = makeInsertCmdObjForRetryableWrite(
            parentLsid, parentTxnNumber, childSessionCmdObj3.stmtId, {_id: 3});
        const retryResForParentSessionCmdObj3 =
            assert.commandWorked(shard0TestDB.runCommand(parentSessionCmdObj3));
        assert.eq(retryResForParentSessionCmdObj3.n, 1);

        assert.eq(shard0TestColl.find({_id: 0}).itcount(), 1);
        assert.eq(shard0TestColl.find({_id: 1}).itcount(), 1);
        assert.eq(shard0TestColl.find({_id: 2}).itcount(), 1);
        assert.eq(shard0TestColl.find({_id: 3}).itcount(), 1);

        assert.commandWorked(mongosTestColl.remove({}));
    }

    runTest({isLargeTxn: false});
    runTest({isLargeTxn: true});
}

{
    jsTest.log("Test aborting a prepared retryable internal transaction and retrying the write " +
               "statements executed in the aborted transaction and in a retryable write prior to " +
               "the transaction");
    let {parentLsid, parentTxnNumber, childLsid, childTxnNumber, stmtId} = makeSessionOpts();

    // Execute a write statement in a retryable write.
    const parentSessionCmdObj =
        makeInsertCmdObjForRetryableWrite(parentLsid, parentTxnNumber, stmtId++, {_id: 0});
    const initialResForParentSessionCmdObj =
        assert.commandWorked(shard0TestDB.runCommand(parentSessionCmdObj));

    // Execute additional write statements in a retryable internal transaction.
    const childSessionCmdObj1 =
        Object.assign(makeInsertCmdObjForTransaction(childLsid, childTxnNumber, stmtId++, {_id: 1}),
                      {startTransaction: true});
    assert.commandWorked(shard0TestDB.runCommand(childSessionCmdObj1));
    const childSessionCmdObj2 =
        makeInsertCmdObjForTransaction(childLsid, childTxnNumber, stmtId++, {_id: 2});
    assert.commandWorked(shard0TestDB.runCommand(childSessionCmdObj2));

    // Prepare the transaction.
    const prepareCmdObj = makePrepareTransactionCmdObj(childLsid, childTxnNumber);
    assert.commandWorked(shard0TestDB.adminCommand(prepareCmdObj));

    stepDownShard0Primary();

    // Abort the transaction after failover.
    const abortCmdObj = makeAbortTransactionCmdObj(childLsid, childTxnNumber);
    assert.commandWorked(shard0TestDB.adminCommand(abortCmdObj));

    // Retry all write statements including the one executed as a retryable write and verify
    // that they execute exactly once.
    const retryResForParentSessionCmdObj =
        assert.commandWorked(shard0TestDB.runCommand(parentSessionCmdObj));
    assert.eq(initialResForParentSessionCmdObj.n, retryResForParentSessionCmdObj.n);

    // It is illegal to restart an aborted prepared transaction.
    assert.commandFailedWithCode(shard0TestDB.runCommand(childSessionCmdObj1), 50911);
    // It is illegal to continue an aborted prepared transaction.
    assert.commandFailedWithCode(shard0TestDB.runCommand(childSessionCmdObj2),
                                 ErrorCodes.NoSuchTransaction);

    assert.eq(shard0TestColl.find({_id: 0}).itcount(), 1);
    assert.eq(shard0TestColl.find({_id: 1}).itcount(), 0);
    assert.eq(shard0TestColl.find({_id: 2}).itcount(), 0);

    // If the in-memory execution history for the prepared transaction did not get discarded after
    // the transaction aborted, the write statements in the commands below would not execute, and
    // therefore the docs {_id: 1} and {_id: 2} would not exist.
    const parentSessionCmdObj1 = makeInsertCmdObjForRetryableWrite(
        parentLsid, parentTxnNumber, childSessionCmdObj1.stmtId, {_id: 1});
    const retryResForParentSessionCmdObj1 =
        assert.commandWorked(shard0TestDB.runCommand(parentSessionCmdObj1));
    assert.eq(retryResForParentSessionCmdObj1.n, 1);

    const parentSessionCmdObj2 = makeInsertCmdObjForRetryableWrite(
        parentLsid, parentTxnNumber, childSessionCmdObj2.stmtId, {_id: 2});
    const retryResForParentSessionCmdObj2 =
        assert.commandWorked(shard0TestDB.runCommand(parentSessionCmdObj2));
    assert.eq(retryResForParentSessionCmdObj2.n, 1);

    assert.eq(shard0TestColl.find({_id: 0}).itcount(), 1);
    assert.eq(shard0TestColl.find({_id: 1}).itcount(), 1);
    assert.eq(shard0TestColl.find({_id: 2}).itcount(), 1);

    assert.commandWorked(mongosTestColl.remove({}));
}

st.stop();
})();
