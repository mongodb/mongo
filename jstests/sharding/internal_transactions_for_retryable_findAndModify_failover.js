/*
 * Tests that retryable internal transactions for findAndModify are retryable across failover.
 *
 * @tags: [requires_fcv_51, featureFlagInternalTransactions]
 */
(function() {
'use strict';

load('jstests/sharding/libs/retryable_internal_transaction_test.js');

const transactionTest = new RetryableInternalTransactionTest();
transactionTest.runTestsForAllRetryableInternalTransactionTypes(
    transactionTest.runFindAndModifyTests, transactionTest.TestMode.kFailover);
transactionTest.stop();
})();
