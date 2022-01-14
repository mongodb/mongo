/*
 * Test that internal transactions cannot be retried while they haven't been committed or aborted.
 *
 * @tags: [requires_fcv_52, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({shards: 1});
let shard0Primary = st.rs0.getPrimary();

const kDbName = "testDb";
const kCollName = "testColl";
const testDB = st.s.getDB(kDbName);
const testColl = testDB.getCollection(kCollName);

assert.commandWorked(testDB.createCollection(kCollName));

const sessionUUID = UUID();
const parentLsid = {
    id: sessionUUID
};
let currentParentTxnNumber = NumberLong(35);

{
    jsTest.log(
        "Test retrying write statement executed in a retryable internal transaction in the " +
        "original internal transaction while the transaction has not been committed or aborted");

    let runTest = ({prepareBeforeRetry, expectedRetryErrorCode}) => {
        const parentTxnNumber = NumberLong(currentParentTxnNumber++);
        const stmtId = NumberInt(1);

        const childLsid = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
        const childTxnNumber = NumberLong(0);
        const originalWriteCmdObj = {
            insert: kCollName,
            documents: [{x: 1}],
            lsid: childLsid,
            txnNumber: childTxnNumber,
            startTransaction: true,
            autocommit: false,
            stmtId: stmtId,
        };
        const commitCmdObj = makeCommitTransactionCmdObj(childLsid, childTxnNumber);

        const retryWriteCmdObj = Object.assign({}, originalWriteCmdObj);

        assert.commandWorked(testDB.runCommand(originalWriteCmdObj));
        if (prepareBeforeRetry) {
            const prepareCmdObj = makePrepareTransactionCmdObj(childLsid, childTxnNumber);
            assert.commandWorked(shard0Primary.adminCommand(prepareCmdObj));
        }

        assert.commandFailedWithCode(testDB.runCommand(retryWriteCmdObj), expectedRetryErrorCode);
        assert.commandFailedWithCode(testDB.adminCommand(commitCmdObj),
                                     ErrorCodes.NoSuchTransaction);
        assert.eq(testColl.count({x: 1}), 0);

        assert.commandWorked(testColl.remove({}));
    };

    runTest({prepareBeforeRetry: false, expectedRetryErrorCode: 5875600});
    runTest({
        prepareBeforeRetry: true,
        expectedRetryErrorCode: ErrorCodes.PreparedTransactionInProgress
    });
}

{
    jsTest.log(
        "Test retrying write statement executed in a retryable internal transaction as a " +
        "retryable write in the parent session while the transaction has not been committed " +
        "or aborted");

    let runTest = ({prepareBeforeRetry}) => {
        const parentTxnNumber = NumberLong(currentParentTxnNumber++);
        const stmtId = NumberInt(1);

        const childLsid = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
        const childTxnNumber = NumberLong(0);
        const originalWriteCmdObj = {
            insert: kCollName,
            documents: [{x: 1}],
            lsid: childLsid,
            txnNumber: childTxnNumber,
            startTransaction: true,
            autocommit: false,
            stmtId: stmtId,
        };
        const commitCmdObj = makeCommitTransactionCmdObj(childLsid, childTxnNumber);

        const retryWriteCmdObj = {
            insert: kCollName,
            documents: [{x: 1}],
            lsid: parentLsid,
            txnNumber: parentTxnNumber,
            stmtIds: [stmtId]
        };

        assert.commandWorked(testDB.runCommand(originalWriteCmdObj));
        if (prepareBeforeRetry) {
            const prepareCmdObj = makePrepareTransactionCmdObj(childLsid, childTxnNumber);
            const preparedTxnRes = assert.commandWorked(shard0Primary.adminCommand(prepareCmdObj));
            commitCmdObj.commitTimestamp = preparedTxnRes.prepareTimestamp;
        }

        assert.commandFailedWithCode(testDB.runCommand(retryWriteCmdObj),
                                     ErrorCodes.RetryableTransactionInProgress);

        if (prepareBeforeRetry) {
            assert.commandWorked(shard0Primary.adminCommand(commitCmdObj));
        }
        assert.commandWorked(testDB.adminCommand(commitCmdObj));
        assert.eq(testColl.count({x: 1}), 1);

        assert.commandWorked(testColl.remove({}));
    };

    runTest({prepareBeforeRetry: false});
    runTest({prepareBeforeRetry: true});
}

{
    jsTest.log(
        "Test retrying write statement executed in a retryable internal transaction in a " +
        "different retryable internal transaction while the original transaction has not been " +
        "committed or aborted");

    let runTest = ({prepareBeforeRetry}) => {
        const parentTxnNumber = NumberLong(currentParentTxnNumber++);
        const stmtId = NumberInt(1);

        const originalChildLsid = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
        const originalChildTxnNumber = NumberLong(0);
        const originalWriteCmdObj = {
            insert: kCollName,
            documents: [{x: 1}],
            lsid: originalChildLsid,
            txnNumber: originalChildTxnNumber,
            startTransaction: true,
            autocommit: false,
            stmtId: stmtId,
        };
        const commitCmdObj = makeCommitTransactionCmdObj(originalChildLsid, originalChildTxnNumber);

        const retryChildLsid = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
        const retryChildTxnNumber = NumberLong(0);
        const retryWriteCmdObj = {
            insert: kCollName,
            documents: [{x: 1}],
            lsid: retryChildLsid,
            txnNumber: retryChildTxnNumber,
            startTransaction: true,
            autocommit: false,
            stmtId: stmtId,
        };

        assert.commandWorked(testDB.runCommand(originalWriteCmdObj));
        if (prepareBeforeRetry) {
            const prepareCmdObj =
                makePrepareTransactionCmdObj(originalChildLsid, originalChildTxnNumber);
            const preparedTxnRes = assert.commandWorked(shard0Primary.adminCommand(prepareCmdObj));
            commitCmdObj.commitTimestamp = preparedTxnRes.prepareTimestamp;
        }

        assert.commandFailedWithCode(testDB.runCommand(retryWriteCmdObj),
                                     ErrorCodes.RetryableTransactionInProgress);

        if (prepareBeforeRetry) {
            assert.commandWorked(shard0Primary.adminCommand(commitCmdObj));
        }
        assert.commandWorked(testDB.adminCommand(commitCmdObj));
        assert.eq(testColl.count({x: 1}), 1);

        assert.commandWorked(testColl.remove({}));
    };

    runTest({prepareBeforeRetry: false});
    runTest({prepareBeforeRetry: true});
}

st.stop();
})();
