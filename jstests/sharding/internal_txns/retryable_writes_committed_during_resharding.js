/**
 * Tests that retryable insert, update and delete statements that are executed inside internal
 * transactions that start and commit on the donor(s) during resharding are retryable on the
 * recipient after resharding.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence, exclude_from_large_txns]
 */
(function() {
"use strict";

load("jstests/sharding/internal_txns/libs/resharding_test.js");

const abortOnInitialTry = false;

{
    const transactionTest = new InternalTransactionReshardingTest({reshardInPlace: false});
    transactionTest.runTestForInsertUpdateDeleteDuringResharding(
        transactionTest.InternalTxnType.kRetryable, abortOnInitialTry);
    transactionTest.stop();
}

{
    const transactionTest = new InternalTransactionReshardingTest({reshardInPlace: true});
    transactionTest.runTestForInsertUpdateDeleteDuringResharding(
        transactionTest.InternalTxnType.kRetryable, abortOnInitialTry);
    transactionTest.stop();
}
})();
