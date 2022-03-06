/**
 * Tests that retryable insert, update and delete statements that are executed inside internal
 * transactions that start and abort on the donor(s) before resharding are not retryable on the
 * recipient after resharding.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/internal_transaction_resharding_test.js");

const transactionTest = new InternalTransactionReshardingTest({reshardInPlace: false});
const abortOnInitialTry = true;
transactionTest.runTestForInsertUpdateDeleteBeforeResharding(
    transactionTest.InternalTxnType.kRetryable, abortOnInitialTry);
transactionTest.stop();
})();
