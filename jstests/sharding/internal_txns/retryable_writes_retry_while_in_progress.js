/*
 * Test that retryable internal transactions cannot be retried while they are still open (i.e. not
 * committed or aborted).
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({shards: 1});
let shard0Primary = st.rs0.getPrimary();

const kDbName = "testDb";
const kCollName = "testColl";
const testDB = shard0Primary.getDB(kDbName);
const testColl = testDB.getCollection(kCollName);

assert.commandWorked(testDB.createCollection(kCollName));

const parentLsid = {
    id: UUID()
};
let currentParentTxnNumber = 35;

function runTest({prepareBeforeRetry}) {
    const parentTxnNumber = currentParentTxnNumber++;
    const docToInsert = {x: 1};
    const stmtId = 1;

    // Initialize initial and retry commands.
    const childLsid = {id: parentLsid.id, txnNumber: NumberLong(parentTxnNumber), txnUUID: UUID()};
    const childTxnNumber = 0;
    const originalWriteCmdObj = {
        insert: kCollName,
        documents: [docToInsert],
        lsid: childLsid,
        txnNumber: NumberLong(childTxnNumber),
        startTransaction: true,
        autocommit: false,
        stmtId: NumberInt(stmtId),
    };
    const commitCmdObj = makeCommitTransactionCmdObj(childLsid, childTxnNumber);
    const retryWriteCmdObj = Object.assign({}, originalWriteCmdObj);

    // Start a retryable internal transaction.
    assert.commandWorked(testDB.runCommand(originalWriteCmdObj));
    if (prepareBeforeRetry) {
        const prepareCmdObj = makePrepareTransactionCmdObj(childLsid, childTxnNumber);
        const prepareTxnRes = assert.commandWorked(shard0Primary.adminCommand(prepareCmdObj));
        commitCmdObj.commitTimestamp = prepareTxnRes.prepareTimestamp;
    }

    // Retry should fail right away.
    assert.commandFailedWithCode(testDB.runCommand(retryWriteCmdObj), 50911);

    // Verify that the transaction can be committed, and that the write statement executed exactly
    // once despite the retry.
    assert.commandWorked(testDB.adminCommand(commitCmdObj));
    assert.eq(testColl.count({x: 1}), 1);

    assert.commandWorked(testColl.remove({}));
}

jsTest.log("Test retrying write statement executed in a retryable internal transaction in the " +
           "original internal transaction while the transaction has not been committed or aborted");

runTest({prepareBeforeRetry: false});
runTest({prepareBeforeRetry: true});

st.stop();
})();
