/*
 * Tests that retryable internal transactions for insert, update and delete are retryable across
 * failover.
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
transactionTest.runTestsForAllRetryableInternalTransactionTypes(
    transactionTest.runInsertUpdateDeleteTests, transactionTest.TestMode.kFailover);
transactionTest.stop();
})();
