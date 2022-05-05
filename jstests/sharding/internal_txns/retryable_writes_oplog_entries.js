/*
 * Tests that the stmtIds for write statements in an internal transaction for retryable writes
 * are stored in the individual operation entries in the applyOps oplog entry for the transaction.
 *
 * Exclude this test from large_txn variants because the variant enforces that the max transaction
 * oplog entry length is 2 operations, and the oplog length assertions in this test do not account
 * for this. We are not losing test coverage as this test inherently tests large transactionss.
 *
 * @tags: [requires_fcv_60, uses_transactions, exclude_from_large_txns]
 */
(function() {
'use strict';

load('jstests/sharding/libs/sharded_transactions_helpers.js');

const kDbName = "testDb";
const kCollName = "testColl";

const st = new ShardingTest({shards: 1});
const mongosTestDB = st.s.getDB(kDbName);
const mongosTestColl = mongosTestDB.getCollection(kCollName);

const kStmtIdsOption = {
    isComplete: 1,
    isIncomplete: 2,
    isRepeated: 3
};

/*
 * Returns an array of NumberInts (i.e. stmtId type) based on the specified option:
 * - If option is 'isComplete', returns [NumberInt(0), ..., NumberInt((numStmtIds-1)*10)].
 * - If option is 'isIncomplete', returns [NumberInt(-1), NumberInt(1), ...,
 * NumberInt(numStmtIds-1)].
 * - If option is 'isRepeated', returns [NumberInt(1), ..., NumberInt(1)].
 */
function makeCustomStmtIdsForTest(numStmtIds, option) {
    switch (option) {
        case kStmtIdsOption.isComplete:
            return [...Array(numStmtIds).keys()].map(i => NumberInt(i * 10));
        case kStmtIdsOption.isIncomplete:
            let stmtIds = [...Array(numStmtIds).keys()];
            stmtIds[0] = -1;
            return stmtIds.map(i => NumberInt(i));
        case kStmtIdsOption.isRepeated:
            return Array(numStmtIds).fill(1).map(i => NumberInt(i));
    }
}

function verifyOplogEntries(cmdObj,
                            lsid,
                            txnNumber,
                            numWriteStatements,
                            {shouldStoreStmtIds, customStmtIdsOption, isPreparedTransaction}) {
    const writeCmdObj = Object.assign(cmdObj, {
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    });
    let stmtIds = null;
    if (customStmtIdsOption) {
        stmtIds = makeCustomStmtIdsForTest(numWriteStatements, customStmtIdsOption);
        writeCmdObj.stmtIds = stmtIds;
    }
    const commitCmdObj = makeCommitTransactionCmdObj(lsid, txnNumber);

    const writeRes = mongosTestDB.runCommand(writeCmdObj);
    if (customStmtIdsOption == kStmtIdsOption.isRepeated) {
        assert.commandFailedWithCode(writeRes, 5875600);
        assert.commandWorked(mongosTestColl.remove({}));
        return;
    }
    assert.commandWorked(writeRes);
    if (isPreparedTransaction) {
        const shard0Primary = st.rs0.getPrimary();
        const prepareCmdObj = makePrepareTransactionCmdObj(lsid, txnNumber);
        const isPreparedTransactionRes =
            assert.commandWorked(shard0Primary.adminCommand(prepareCmdObj));
        commitCmdObj.commitTimestamp = isPreparedTransactionRes.prepareTimestamp;
        assert.commandWorked(shard0Primary.adminCommand(commitCmdObj));
    }
    assert.commandWorked(mongosTestDB.adminCommand(commitCmdObj));

    const oplogEntries = getOplogEntriesForTxn(st.rs0, lsid, txnNumber);
    assert.eq(oplogEntries.length, isPreparedTransaction ? 2 : 1, oplogEntries);

    const applyOpsOplogEntry = oplogEntries[0];
    assert(!applyOpsOplogEntry.hasOwnProperty("stmtId"));
    const operations = applyOpsOplogEntry.o.applyOps;
    operations.forEach((operation, index) => {
        if (shouldStoreStmtIds) {
            const operationStmtId = stmtIds ? stmtIds[index] : index;
            if (operationStmtId == -1) {
                // Uninitialized stmtIds should be ignored.
                assert(!operation.hasOwnProperty("stmtId"), operation);
            } else {
                assert.eq(operation.stmtId, operationStmtId, operation);
            }
        } else {
            assert(!operation.hasOwnProperty("stmtId"), operation);
        }
    });

    if (isPreparedTransaction) {
        const commitOplogEntry = oplogEntries[1];
        assert(!commitOplogEntry.hasOwnProperty("stmtId"));
    }

    assert.commandWorked(mongosTestColl.remove({}));
}

function testInserts(lsid, txnNumber, testOptions) {
    jsTest.log("Test batched inserts");
    const insertCmdObj = {
        insert: kCollName,
        documents: [{_id: 0, x: 0}, {_id: 1, x: 1}],
    };
    verifyOplogEntries(insertCmdObj, lsid, txnNumber, insertCmdObj.documents.length, testOptions);
}

function testUpdates(lsid, txnNumber, testOptions) {
    jsTest.log("Test batched updates");
    assert.commandWorked(mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 1}]));
    const updateCmdObj = {
        update: kCollName,
        updates: [
            {q: {_id: 0, x: 0}, u: {$inc: {x: -10}}},
            {q: {_id: 1, x: 1}, u: {$inc: {x: 10}}},
        ],
    };
    verifyOplogEntries(updateCmdObj, lsid, txnNumber, updateCmdObj.updates.length, testOptions);
}

function testDeletes(lsid, txnNumber, testOptions) {
    jsTest.log("Test batched deletes");
    assert.commandWorked(mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 1}]));
    const deleteCmdObj = {
        delete: kCollName,
        deletes: [
            {q: {_id: 0, x: 0}, limit: 1},
            {q: {_id: 1, x: 1}, limit: 1},
        ],
    };
    verifyOplogEntries(deleteCmdObj, lsid, txnNumber, deleteCmdObj.deletes.length, testOptions);
}

{
    jsTest.log("Test that oplog entries for non-internal transactions do not have stmtIds");
    const lsid = {id: UUID()};
    let txnNumber = 0;
    const testOptions = {shouldStoreStmtIds: false};
    testInserts(lsid, txnNumber++, testOptions);
    testUpdates(lsid, txnNumber++, testOptions);
    testDeletes(lsid, txnNumber++, testOptions);
}

{
    jsTest.log(
        "Test that oplog entries for non-retryable internal transactions do not have stmtIds");
    const lsid = {id: UUID(), txnUUID: UUID()};
    let txnNumber = 0;
    const testOptions = {shouldStoreStmtIds: false};
    testInserts(lsid, txnNumber++, testOptions);
    testUpdates(lsid, txnNumber++, testOptions);
    testDeletes(lsid, txnNumber++, testOptions);
}

{
    jsTest.log("Test that oplog entries for retryable internal transactions have stmtIds");
    const lsid = {id: UUID(), txnNumber: NumberLong(0), txnUUID: UUID()};
    let txnNumber = 0;

    let runTests = ({isPreparedTransaction}) => {
        jsTest.log("Test prepared transactions: " + isPreparedTransaction);

        jsTest.log("Test with default stmtIds");
        const testOptions0 = {shouldStoreStmtIds: true, isPreparedTransaction};
        testInserts(lsid, txnNumber++, testOptions0);
        testUpdates(lsid, txnNumber++, testOptions0);
        testDeletes(lsid, txnNumber++, testOptions0);

        jsTest.log("Test with custom and valid stmtIds");
        const testOptions1 = {
            shouldStoreStmtIds: true,
            customStmtIdsOption: kStmtIdsOption.isComplete,
            isPreparedTransaction
        };
        testInserts(lsid, txnNumber++, testOptions1);
        testUpdates(lsid, txnNumber++, testOptions1);
        testDeletes(lsid, txnNumber++, testOptions1);

        jsTest.log(
            "Test with custom stmtIds containing -1. Verify that operation entries for write " +
            "statements with stmtId=-1 do not have a 'stmtId' field");
        const testOptions2 = {
            shouldStoreStmtIds: true,
            customStmtIdsOption: kStmtIdsOption.isIncomplete,
            isPreparedTransaction
        };
        testInserts(lsid, txnNumber++, testOptions2);
        testUpdates(lsid, txnNumber++, testOptions2);
        testDeletes(lsid, txnNumber++, testOptions2);

        jsTest.log(
            "Test with custom stmtIds containing repeats. Verify that the command fails with " +
            "a uassert instead causes the mongod that executes it to crash");
        const testOptions3 = {
            shouldStoreStmtIds: true,
            customStmtIdsOption: kStmtIdsOption.isRepeated,
            isPreparedTransaction
        };
        testInserts(lsid, txnNumber++, testOptions3);
        testUpdates(lsid, txnNumber++, testOptions3);
        testDeletes(lsid, txnNumber++, testOptions3);
    };

    runTests({isPreparedTransaction: false});
    runTests({isPreparedTransaction: true});
}

st.stop();
})();
