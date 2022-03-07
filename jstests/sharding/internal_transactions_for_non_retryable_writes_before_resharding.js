/**
 * Tests that resharding does not transfer the history for non-retryable internal transactions that
 * commit or abort on the donor(s) before resharding to the recipient.
 *
 * TODO (SERVER-63877): Determine if resharding should migrate internal sessions for non-retryable
 * writes.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/internal_transaction_resharding_test.js");

{
    const transactionTest = new InternalTransactionReshardingTest({reshardInPlace: false});
    transactionTest.runTestForInsertUpdateDeleteBeforeResharding(
        transactionTest.InternalTxnType.kNonRetryable, false /* abortOnInitialTry */);
    transactionTest.stop();
}

{
    const transactionTest = new InternalTransactionReshardingTest({reshardInPlace: false});
    transactionTest.runTestForInsertUpdateDeleteBeforeResharding(
        transactionTest.InternalTxnType.kNonRetryable, true /* abortOnInitialTry */);
    transactionTest.stop();
}
})();
