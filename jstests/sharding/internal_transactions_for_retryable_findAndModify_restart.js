/*
 * Tests that retryable internal transactions for findAndModify are retryable across restart.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
'use strict';

load('jstests/sharding/libs/retryable_internal_transaction_test.js');

const transactionTest = new RetryableInternalTransactionTest();
transactionTest.runTestsForAllRetryableInternalTransactionTypes(
    transactionTest.runFindAndModifyTests, transactionTest.TestMode.kRestart);
transactionTest.stop();
})();
