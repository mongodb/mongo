/**
 * Tests that retryable insert, update and delete statements that are executed inside internal
 * transactions that start and commit on the donor(s) before resharding are not retryable on the
 * recipient after resharding.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence, exclude_from_large_txns]
 */
(function() {
"use strict";

load("jstests/sharding/internal_txns/libs/resharding_test.js");

const transactionTest = new InternalTransactionReshardingTest({reshardInPlace: false});
transactionTest.runTestForInsertUpdateDeleteBeforeResharding(
    transactionTest.InternalTxnType.kRetryable, false /* abortOnInitialTry */);
transactionTest.stop();
})();
