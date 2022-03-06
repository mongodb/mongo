/**
 * Tests that retryable insert, update and delete statements that are executed inside internal
 * transactions that start and abort on the donor(s) during resharding are retryable on the
 * recipient after resharding.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/internal_transaction_resharding_test.js");

const abortOnInitialTry = true;

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
