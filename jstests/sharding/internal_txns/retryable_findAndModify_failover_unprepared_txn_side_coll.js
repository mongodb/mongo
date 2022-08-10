/*
 * Tests that unprepared retryable internal transactions for findAndModify on a shard with image
 * collection enabled are retryable across failover.
 *
 * @tags: [requires_fcv_60, uses_transactions, exclude_from_large_txns]
 */
(function() {
'use strict';

load("jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js");

const transactionTest = new RetryableInternalTransactionTest();
transactionTest.runTestsForAllUnpreparedRetryableInternalTransactionTypes(
    transactionTest.runFindAndModifyTestsEnableImageCollection, transactionTest.TestMode.kFailover);
transactionTest.stop();
})();
