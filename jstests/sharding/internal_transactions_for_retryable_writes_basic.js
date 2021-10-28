/*
 * Tests that internal transactions for retryable writes can be retried.
 *
 * @tags: [requires_fcv_51, featureFlagInternalTransactions]
 */
(function() {
'use strict';

load('jstests/sharding/libs/sharded_transactions_helpers.js');

const kDbName = "testDb";
const kCollName = "testColl";

// For creating documents that will result in large transactions.
const kSize10MB = 10 * 1024 * 1024;

const st = new ShardingTest({shards: 1});

const mongosTestDB = st.s.getDB(kDbName);
const mongosTestColl = mongosTestDB.getCollection(kCollName);

assert.commandWorked(mongosTestDB.createCollection(kCollName));

function testRetry(
    cmdObj, lsid, txnNumber, {shouldRetrySucceed, isPreparedTransaction, restart, checkFunc}) {
    const writeCmdObj = Object.assign(cmdObj, {
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    });
    const commitCmdObj = makeCommitTransactionCmdObj(lsid, txnNumber);

    const initialRes = assert.commandWorked(mongosTestDB.runCommand(writeCmdObj));
    if (isPreparedTransaction) {
        const shard0Primary = st.rs0.getPrimary();
        const prepareCmdObj = makePrepareTransactionCmdObj(lsid, txnNumber);
        const isPreparedTransactionRes =
            assert.commandWorked(shard0Primary.adminCommand(prepareCmdObj));
        commitCmdObj.commitTimestamp = isPreparedTransactionRes.prepareTimestamp;
        assert.commandWorked(shard0Primary.adminCommand(commitCmdObj));
    }
    assert.commandWorked(mongosTestDB.adminCommand(commitCmdObj));

    const oplogEntriesBeforeRetry = getOplogEntriesForTxn(st.rs0, lsid, txnNumber);
    const txnEntriesBeforeRetry = getTxnEntriesForSession(st.rs0, lsid);
    assert.eq(
        oplogEntriesBeforeRetry.length, isPreparedTransaction ? 2 : 1, oplogEntriesBeforeRetry);

    if (restart) {
        st.rs0.stopSet(null /* signal */, true /*forRestart */);
        st.rs0.startSet({restart: true});
    }

    const retryRes = mongosTestDB.runCommand(writeCmdObj);
    if (shouldRetrySucceed) {
        assert.commandWorked(retryRes);
        checkFunc(initialRes, retryRes);
        assert.commandWorked(mongosTestDB.adminCommand(commitCmdObj));
    } else {
        assert.commandFailedWithCode(retryRes, ErrorCodes.ConflictingOperationInProgress);
    }

    const oplogEntriesAfterRetry = getOplogEntriesForTxn(st.rs0, lsid, txnNumber);
    const txnEntriesAfterRetry = getTxnEntriesForSession(st.rs0, lsid);
    assert.eq(oplogEntriesBeforeRetry, oplogEntriesAfterRetry);
    assert.eq(txnEntriesBeforeRetry, txnEntriesAfterRetry);

    assert.commandWorked(mongosTestColl.remove({}));
}

function testRetryLargeTxn(lsid, txnNumber, {isPreparedTransaction, restart}) {
    jsTest.log(
        "Test retrying a retryable internal transaction with more than one applyOps oplog entry");

    let stmtId = 1;
    let makeInsertCmdObj = (doc) => {
        return {
            insert: kCollName,
            documents: [Object.assign(doc, {y: new Array(kSize10MB).join("a")})],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(stmtId++),
            autocommit: false
        };
    };
    const insertCmdObj0 = Object.assign(makeInsertCmdObj({_id: 0, x: 0}), {startTransaction: true});
    const insertCmdObj1 = makeInsertCmdObj({_id: 1, x: 2});
    const insertCmdObj2 = makeInsertCmdObj({_id: 3, x: 3});
    const commitCmdObj = makeCommitTransactionCmdObj(lsid, txnNumber);

    const insertCmdObjs = [insertCmdObj0, insertCmdObj1, insertCmdObj2];
    assert.commandWorked(mongosTestDB.runCommand(insertCmdObj0));
    assert.commandWorked(mongosTestDB.runCommand(insertCmdObj1));
    assert.commandWorked(mongosTestDB.runCommand(insertCmdObj2));
    if (isPreparedTransaction) {
        const shard0Primary = st.rs0.getPrimary();
        const prepareCmdObj = makePrepareTransactionCmdObj(lsid, txnNumber);
        const isPreparedTransactionRes =
            assert.commandWorked(shard0Primary.adminCommand(prepareCmdObj));
        commitCmdObj.commitTimestamp = isPreparedTransactionRes.prepareTimestamp;
        assert.commandWorked(shard0Primary.adminCommand(commitCmdObj));
    }
    assert.commandWorked(mongosTestDB.adminCommand(commitCmdObj));

    const oplogEntriesBeforeRetry = getOplogEntriesForTxn(st.rs0, lsid, txnNumber);
    const txnEntriesBeforeRetry = getTxnEntriesForSession(st.rs0, lsid);
    assert.eq(oplogEntriesBeforeRetry.length,
              isPreparedTransaction ? insertCmdObjs.length + 1 : insertCmdObjs.length,
              oplogEntriesBeforeRetry);

    if (restart) {
        st.rs0.stopSet(null /* signal */, true /*forRestart */);
        st.rs0.startSet({restart: true});
    }

    insertCmdObjs.forEach(insertCmdObj => {
        const retryRes = assert.commandWorked(mongosTestDB.runCommand(insertCmdObj));
        assert.eq(retryRes.n, 1);
    });
    assert.commandWorked(mongosTestDB.adminCommand(commitCmdObj));

    const oplogEntriesAfterRetry = getOplogEntriesForTxn(st.rs0, lsid, txnNumber);
    const txnEntriesAfterRetry = getTxnEntriesForSession(st.rs0, lsid);
    assert.eq(oplogEntriesBeforeRetry, oplogEntriesAfterRetry);
    assert.eq(txnEntriesBeforeRetry, txnEntriesAfterRetry);

    assert.eq(mongosTestColl.count(), insertCmdObjs.length);
    assert.commandWorked(mongosTestColl.remove({}));
}

function testRetryInserts(lsid, txnNumber, {shouldRetrySucceed, isPreparedTransaction, restart}) {
    jsTest.log("Test batched inserts");

    const insertCmdObj = {
        insert: kCollName,
        documents: [{_id: 0, x: 0}, {_id: 1, x: 1}],
    };
    const checkFunc = (initialRes, retryRes) => {
        assert.eq(initialRes.n, retryRes.n);
        insertCmdObj.documents.forEach(doc => {
            assert.eq(mongosTestColl.count(doc), 1);
        });
    };
    testRetry(insertCmdObj,
              lsid,
              txnNumber,
              {shouldRetrySucceed, isPreparedTransaction, restart, checkFunc});
}

function testRetryUpdates(lsid, txnNumber, {shouldRetrySucceed, isPreparedTransaction, restart}) {
    jsTest.log("Test batched updates");

    assert.commandWorked(mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 1}]));

    const updateCmdObj = {
        update: kCollName,
        updates: [{q: {_id: 0, x: 0}, u: {$inc: {x: 10}}}, {q: {_id: 1, x: 1}, u: {$inc: {x: 10}}}],
    };
    const checkFunc = (initialRes, retryRes) => {
        assert.eq(initialRes.nModified, retryRes.nModified);
        updateCmdObj.updates.forEach(updateArgs => {
            const originalDoc = updateArgs.q;
            const updatedDoc = Object.assign({}, updateArgs.q);
            updatedDoc.x += updateArgs.u.$inc.x;
            assert.eq(mongosTestColl.count(originalDoc), 0);
            assert.eq(mongosTestColl.count(updatedDoc), 1);
        });
    };

    testRetry(updateCmdObj,
              lsid,
              txnNumber,
              {shouldRetrySucceed, isPreparedTransaction, restart, checkFunc});
}

function testRetryDeletes(lsid, txnNumber, {shouldRetrySucceed, isPreparedTransaction, restart}) {
    jsTest.log("Test batched deletes");

    assert.commandWorked(mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 1}]));

    const deleteCmdObj = {
        delete: kCollName,
        deletes: [{q: {_id: 0, x: 0}, limit: 0}, {q: {_id: 1, x: 1}, limit: 0}],
    };
    const checkFunc = (initialRes, retryRes) => {
        assert.eq(initialRes.n, retryRes.n);
        deleteCmdObj.deletes.forEach(deleteArgs => {
            assert.eq(mongosTestColl.count(deleteArgs.q), 0);
        });
    };

    testRetry(deleteCmdObj,
              lsid,
              txnNumber,
              {shouldRetrySucceed, isPreparedTransaction, restart, checkFunc});
}

{
    jsTest.log("Test that non-internal transactions cannot be retried");
    const lsid = {id: UUID()};
    let txnNumber = 0;

    const shouldRetrySucceed = false;
    testRetryInserts(lsid, txnNumber++, {shouldRetrySucceed});
    testRetryUpdates(lsid, txnNumber++, {shouldRetrySucceed});
    testRetryDeletes(lsid, txnNumber++, {shouldRetrySucceed});
}

{
    jsTest.log("Test that non-retryable internal transactions cannot be retried");
    const lsid = {id: UUID(), txnUUID: UUID()};
    let txnNumber = 0;

    const shouldRetrySucceed = false;
    testRetryInserts(lsid, txnNumber++, {shouldRetrySucceed});
    testRetryUpdates(lsid, txnNumber++, {shouldRetrySucceed});
    testRetryDeletes(lsid, txnNumber++, {shouldRetrySucceed});
}

{
    jsTest.log("Test that retryable internal transactions can be retried");
    const lsid = {id: UUID(), txnNumber: NumberLong(0), txnUUID: UUID()};
    let txnNumber = 0;

    let runTests = ({isPreparedTransaction, restart}) => {
        jsTest.log("Test prepared transactions: " + isPreparedTransaction);
        const testOptions = {shouldRetrySucceed: true, isPreparedTransaction, restart};
        testRetryInserts(lsid, txnNumber++, testOptions);
        testRetryUpdates(lsid, txnNumber++, testOptions);
        testRetryDeletes(lsid, txnNumber++, testOptions);
        testRetryLargeTxn(lsid, txnNumber++, testOptions);
    };

    runTests({isPreparedTransaction: false});
    runTests({isPreparedTransaction: true});
    runTests({isPreparedTransaction: false, restart: true});
    runTests({isPreparedTransaction: true, restart: true});
}

st.stop();
})();
