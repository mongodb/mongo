/*
 * Tests that retryable internal transactions are retryable and that other transactions are not.
 *
 * @tags: [requires_fcv_51, featureFlagInternalTransactions]
 */
(function() {
'use strict';

load('jstests/sharding/libs/retryable_internal_transaction_test.js');

const transactionTest = new RetryableInternalTransactionTest();

{
    jsTest.log("Test that non-internal transactions cannot be retried");
    const lsid = {id: UUID()};
    const testOptions = {expectRetryToSucceed: false};
    transactionTest.runInsertUpdateDeleteTests(lsid, testOptions);
    transactionTest.runFindAndModifyTests(lsid, testOptions);
}

{
    jsTest.log("Test that non-retryable internal transactions cannot be retried");
    const lsid = {id: UUID(), txnUUID: UUID()};
    const testOptions = {expectRetryToSucceed: false};
    transactionTest.runInsertUpdateDeleteTests(lsid, testOptions);
    transactionTest.runFindAndModifyTests(lsid, testOptions);
}

{
    jsTest.log("Test that retryable internal transactions can be retried");
    transactionTest.runTestsForAllRetryableInternalTransactionTypes(
        transactionTest.runInsertUpdateDeleteTests);
    transactionTest.runTestsForAllRetryableInternalTransactionTypes(
        transactionTest.runFindAndModifyTests);
}

transactionTest.stop();
})();
