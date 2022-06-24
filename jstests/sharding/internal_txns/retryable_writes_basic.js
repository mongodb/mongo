/*
 * Tests that retryable internal transactions for insert, update and delete are retryable and
 * other kinds of transactions for insert, update and delete are not retryable.
 *
 * Exclude this test from large_txn variants because the variant enforces that the max transaction
 * oplog entry length is 2 operations, and the oplog length assertions in this test do not account
 * for this. We are not losing test coverage as this test inherently tests large transactions.
 *
 * @tags: [requires_fcv_60, uses_transactions, exclude_from_large_txns]
 */
(function() {
'use strict';

load("jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js");

const transactionTest = new RetryableInternalTransactionTest();

{
    jsTest.log("Test that non-internal transactions cannot be retried");
    const makeSessionIdFunc = () => {
        return {id: UUID()};
    };
    const expectRetryToSucceed = false;
    transactionTest.runInsertUpdateDeleteTests(
        {txnOptions: {makeSessionIdFunc}, expectRetryToSucceed});
}

{
    jsTest.log("Test that non-retryable internal transactions cannot be retried");
    const makeSessionIdFunc = () => {
        return {id: UUID(), txnUUID: UUID()};
    };
    const expectRetryToSucceed = false;
    transactionTest.runInsertUpdateDeleteTests(
        {txnOptions: {makeSessionIdFunc}, expectRetryToSucceed});
}

{
    jsTest.log("Test that retryable internal transactions can be retried");
    transactionTest.runTestsForAllRetryableInternalTransactionTypes(
        transactionTest.runInsertUpdateDeleteTests, transactionTest.TestMode.kNonRecovery);
}

{
    jsTest.log("Test multi writes are allowed in non retryable transactions");
    transactionTest.testNonRetryableTxnMultiWrites();
}

{
    jsTest.log(
        "Test multi writes with an initialized statement id are rejected in retryable transactions");
    transactionTest.testRetryableTxnMultiWrites();
}

transactionTest.stop();
})();
