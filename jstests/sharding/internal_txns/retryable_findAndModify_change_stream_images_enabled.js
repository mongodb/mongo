/**
 * Tests that retryable internal transactions for "findAndModify" commands against collection with
 * changeStreamPreAndPostImages enabled are retryable.
 *
 * @tags: [
 * requires_fcv_60,
 * uses_transactions,
 * ]
 */
(function() {
'use strict';

load("jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js");

const transactionTest =
    new RetryableInternalTransactionTest({changeStreamPreAndPostImages: {enabled: true}});
transactionTest.runTestsForAllRetryableInternalTransactionTypes(
    transactionTest.runFindAndModifyTestsEnableImageCollection);
transactionTest.runTestsForAllRetryableInternalTransactionTypes(
    transactionTest.runFindAndModifyTestsDisableImageCollection);
transactionTest.stop();
})();
