/*
 * Tests that retryable internal transactions for insert, update and delete are retryable across
 * failover.
 *
 * @tags: [requires_fcv_51, featureFlagInternalTransactions]
 */
(function() {
'use strict';

load("jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js");

const transactionTest = new RetryableInternalTransactionTest();
transactionTest.runTestsForAllRetryableInternalTransactionTypes(
    transactionTest.runInsertUpdateDeleteTests, transactionTest.TestMode.kFailover);
transactionTest.stop();
})();
