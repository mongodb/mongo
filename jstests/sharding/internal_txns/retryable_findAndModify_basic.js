/*
 * Tests that retryable internal transactions for findAndModify are retryable and other kinds of
 * transactions for findAndModify are not retryable.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
'use strict';

load('jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js');

const transactionTest = new RetryableInternalTransactionTest();

{
    jsTest.log("Test that non-internal transactions cannot be retried");
    const lsid = {id: UUID()};
    const testOptions = {expectRetryToSucceed: false};
    transactionTest.runFindAndModifyTestsEnableImageCollection(lsid, testOptions);
}

{
    jsTest.log("Test that non-retryable internal transactions cannot be retried");
    const lsid = {id: UUID(), txnUUID: UUID()};
    const testOptions = {expectRetryToSucceed: false};
    transactionTest.runFindAndModifyTestsEnableImageCollection(lsid, testOptions);
}

{
    jsTest.log("Test that retryable internal transactions can be retried");
    transactionTest.runTestsForAllRetryableInternalTransactionTypes(
        transactionTest.runFindAndModifyTestsEnableImageCollection,
        transactionTest.TestMode.kNonRecovery);
    transactionTest.runTestsForAllRetryableInternalTransactionTypes(
        transactionTest.runFindAndModifyTestsDisableImageCollection,
        transactionTest.TestMode.kNonRecovery);
}

transactionTest.stop();
})();
