/*
 * Tests that retryable internal transactions for findAndModify are retryable and other kinds of
 * transactions for findAndModify are not retryable.
 *
 * @tags: [requires_fcv_60, uses_transactions, exclude_from_large_txns]
 */
(function() {
'use strict';

load('jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js');

const transactionTest = new RetryableInternalTransactionTest();

{
    jsTest.log("Test that non-internal transactions cannot be retried");
    const makeSessionIdFunc = () => {
        return {id: UUID()};
    };
    const expectRetryToSucceed = false;
    transactionTest.runFindAndModifyTestsEnableImageCollection(
        {txnOptions: {makeSessionIdFunc}, expectRetryToSucceed});
}

{
    jsTest.log("Test that non-retryable internal transactions cannot be retried");
    const makeSessionIdFunc = () => {
        return {id: UUID(), txnUUID: UUID()};
    };
    const expectRetryToSucceed = false;
    transactionTest.runFindAndModifyTestsEnableImageCollection(
        {txnOptions: {makeSessionIdFunc}, expectRetryToSucceed});
}

{
    jsTest.log("Test that retryable internal transactions can be retried");
    transactionTest.runTestsForAllRetryableInternalTransactionTypes(
        transactionTest.runFindAndModifyTestsEnableImageCollection,
        transactionTest.TestMode.kNonRecovery);
}

transactionTest.stop();
})();
