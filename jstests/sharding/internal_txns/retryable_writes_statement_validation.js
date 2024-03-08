/*
 * Test that a client cannot add write statements to commit or prepared a retryable internal
 * transaction, or overwrite previously executed write statements in an in-progress retryable
 * internal transaction.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    getOplogEntriesForTxn,
    getTxnEntriesForSession,
    makeCommitTransactionCmdObj,
    makePrepareTransactionCmdObj,
} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const kDbName = "testDb";
const kCollName = "testColl";

const st = new ShardingTest({shards: 1});
const shard0Primary = st.rs0.getPrimary();

const mongosTestDB = st.s.getDB(kDbName);
const mongosTestColl = mongosTestDB.getCollection(kCollName);
const shard0TestDB = shard0Primary.getDB(kDbName);

assert.commandWorked(mongosTestDB.createCollection(kCollName));

function makeInsertCmdObj(docs, lsid, txnNumber, stmtId, startTransaction) {
    const cmdObj = {
        insert: kCollName,
        documents: docs,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        autocommit: false,
    };
    if (startTransaction) {
        cmdObj.startTransaction = true;
    }
    return cmdObj;
}

{
    jsTest.log("Test that retrying a write statement that was previously executed in a " +
               "startTransaction transaction statement before the transaction commits returns " +
               "an error");
    let runTransaction = (db, expectedRetryErrorCode) => {
        const lsid = {id: UUID(), txnNumber: NumberLong(0), txnUUID: UUID()};
        const txnNumber = 0;
        let stmtId = 1;
        const insertCmdObj0 = makeInsertCmdObj([{x: 0}], lsid, txnNumber, stmtId, true);
        const commitCmdObj = makeCommitTransactionCmdObj(lsid, txnNumber);

        assert.commandWorked(db.runCommand(insertCmdObj0));
        assert.commandFailedWithCode(db.runCommand(insertCmdObj0), expectedRetryErrorCode);
        return db.adminCommand(commitCmdObj);
    };

    // For a transaction executed directly against a mongod, the retry will return an error without
    // executing because it is illegal to restart an in-progress transaction.
    const commitRes0 = runTransaction(shard0TestDB, 50911);
    assert.commandWorked(commitRes0);
    assert.eq(mongosTestColl.count(), 1);
    assert.commandWorked(mongosTestColl.remove({}));

    // For a transaction executed against a mongos, the retry will be sent to corresponding shard
    // without the "startTransaction" field since mongos only attaches "startTransaction":true in
    // the first command to each shard. Therefore, the retry will not return an error until after
    // it is executed and after that the transaction will be implicitly aborted.
    // TODO SERVER-87660 Re-enable this test case
    /*const commitRes1 = runTransaction(mongosTestDB, 5875600);
    assert.commandFailedWithCode(commitRes1, ErrorCodes.NoSuchTransaction);
    assert.eq(mongosTestColl.count(), 0);*/
}

{
    jsTest.log("Test that retrying a write statement that was previously executed in a " +
               "non-startTransaction transaction statement before the transaction commits " +
               "returns an error");
    let runTransaction = (db, expectedRetryErrorCode) => {
        const lsid = {id: UUID(), txnNumber: NumberLong(0), txnUUID: UUID()};
        const txnNumber = 0;
        let stmtId = 1;
        const insertCmdObj0 =
            makeInsertCmdObj([{x: 0}], lsid, txnNumber, stmtId++, true /* startTransaction */);
        const insertCmdObj1 = makeInsertCmdObj([{x: 1}], lsid, txnNumber, stmtId++);
        const commitCmdObj = makeCommitTransactionCmdObj(lsid, txnNumber);

        assert.commandWorked(db.runCommand(insertCmdObj0));
        assert.commandWorked(db.runCommand(insertCmdObj1));
        assert.commandFailedWithCode(db.runCommand(insertCmdObj1), expectedRetryErrorCode);
        return db.adminCommand(commitCmdObj);
    };

    // For both transactions executed against a mongod and a mongos, the retry will not return an
    // error until after it is executed and after that the transaction will be implicitly aborted.
    const commitRes0 = runTransaction(shard0TestDB, 5875600);
    assert.commandFailedWithCode(commitRes0, ErrorCodes.NoSuchTransaction);
    assert.eq(mongosTestColl.count(), 0);

    const commitRes1 = runTransaction(mongosTestDB, 5875600);
    assert.commandFailedWithCode(commitRes1, ErrorCodes.NoSuchTransaction);
    assert.eq(mongosTestColl.count(), 0);
}

{
    jsTest.log("Test that running an additional write statement after the transaction has " +
               "committed returns an error and does not modify the transaction");
    let runTest = (db) => {
        const lsid = {id: UUID(), txnNumber: NumberLong(0), txnUUID: UUID()};
        const txnNumber = 0;
        let stmtId = 1;
        const insertCmdObj0 =
            makeInsertCmdObj([{x: 0}], lsid, txnNumber, stmtId++, true /* startTransaction */);
        const insertCmdObj1 = makeInsertCmdObj([{x: 1}], lsid, txnNumber, stmtId++);
        const commitCmdObj = makeCommitTransactionCmdObj(lsid, txnNumber);

        assert.commandWorked(db.runCommand(insertCmdObj0));
        assert.commandWorked(db.adminCommand(commitCmdObj));

        const oplogEntriesBefore = getOplogEntriesForTxn(st.rs0, lsid, txnNumber);
        const txnEntriesBefore = getTxnEntriesForSession(st.rs0, lsid);

        assert.commandFailedWithCode(db.runCommand(insertCmdObj1), 5875603);
        assert.commandWorked(db.adminCommand(commitCmdObj));

        const oplogEntriesAfter = getOplogEntriesForTxn(st.rs0, lsid, txnNumber);
        const txnEntriesAfter = getTxnEntriesForSession(st.rs0, lsid);
        assert.eq(oplogEntriesBefore, oplogEntriesAfter);
        assert.eq(txnEntriesBefore, txnEntriesAfter);

        assert.eq(mongosTestColl.count(), 1);
        assert.commandWorked(mongosTestColl.remove({}));
    };

    runTest(shard0TestDB);
    runTest(mongosTestDB);
}

{
    jsTest.log("Test that running an additional write statement after the transaction has " +
               "prepared returns an error");
    let runTest = (db) => {
        const lsid = {id: UUID(), txnNumber: NumberLong(0), txnUUID: UUID()};
        const txnNumber = 0;
        let stmtId = 1;
        const insertCmdObj0 =
            makeInsertCmdObj([{x: 0}], lsid, txnNumber, stmtId++, true /* startTransaction */);
        const insertCmdObj1 = makeInsertCmdObj([{x: 1}], lsid, txnNumber, stmtId++);
        const prepareCmdObj = makePrepareTransactionCmdObj(lsid, txnNumber);
        const commitCmdObj = makeCommitTransactionCmdObj(lsid, txnNumber);

        assert.commandWorked(db.runCommand(insertCmdObj0));
        const isPreparedTransactionRes =
            assert.commandWorked(shard0TestDB.adminCommand(prepareCmdObj));

        const oplogEntriesBefore = getOplogEntriesForTxn(st.rs0, lsid, txnNumber);
        const txnEntriesBefore = getTxnEntriesForSession(st.rs0, lsid);

        assert.commandFailedWithCode(db.runCommand(insertCmdObj1),
                                     ErrorCodes.PreparedTransactionInProgress);

        const oplogEntriesAfter = getOplogEntriesForTxn(st.rs0, lsid, txnNumber);
        const txnEntriesAfter = getTxnEntriesForSession(st.rs0, lsid);

        if (FixtureHelpers.isMongos(db)) {
            // The PreparedTransactionInProgress error should cause mongos to implicitly abort the
            // transaction.
            assert.neq(oplogEntriesBefore, oplogEntriesAfter);
            assert.neq(txnEntriesBefore, txnEntriesAfter);

            assert.commandFailedWithCode(db.adminCommand(commitCmdObj),
                                         ErrorCodes.NoSuchTransaction);
            assert.eq(mongosTestColl.count(), 0);
        } else {
            assert.eq(oplogEntriesBefore, oplogEntriesAfter);
            assert.eq(txnEntriesBefore, txnEntriesAfter);

            commitCmdObj.commitTimestamp = isPreparedTransactionRes.prepareTimestamp;
            assert.commandWorked(db.adminCommand(commitCmdObj));
            assert.eq(mongosTestColl.count(), 1);
            assert.commandWorked(mongosTestColl.remove({}));
        }
    };

    runTest(shard0TestDB);
    runTest(mongosTestDB);
}

st.stop();
