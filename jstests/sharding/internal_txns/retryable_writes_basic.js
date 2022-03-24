/*
 * Tests that retryable internal transactions for insert, update and delete are retryable and
 * other kinds of transactions for insert, update and delete are not retryable.
 *
 * @tags: [requires_fcv_51, featureFlagInternalTransactions]
 */
(function() {
'use strict';

load("jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js");

const transactionTest = new RetryableInternalTransactionTest();

{
    jsTest.log("Test that non-internal transactions cannot be retried");
    const lsid = {id: UUID()};
    const testOptions = {expectRetryToSucceed: false};
    transactionTest.runInsertUpdateDeleteTests(lsid, testOptions);
}

{
    jsTest.log("Test that non-retryable internal transactions cannot be retried");
    const lsid = {id: UUID(), txnUUID: UUID()};
    const testOptions = {expectRetryToSucceed: false};
    transactionTest.runInsertUpdateDeleteTests(lsid, testOptions);
}

{
    jsTest.log("Test that retryable internal transactions can be retried");
    transactionTest.runTestsForAllRetryableInternalTransactionTypes(
        transactionTest.runInsertUpdateDeleteTests, transactionTest.TestMode.kNonRecovery);
}

transactionTest.stop();
})();
