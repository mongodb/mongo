/**
 * Tests that resharding does not transfer the history for non-retryable internal transactions that
 * commit or abort on the donor(s) during resharding to the recipient.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/internal_txns/libs/resharding_test.js");

{
    const transactionTest = new InternalTransactionReshardingTest({reshardInPlace: false});
    transactionTest.runTestForInsertUpdateDeleteDuringResharding(
        transactionTest.InternalTxnType.kNonRetryable, false /* abortOnInitialTry */);
    transactionTest.stop();
}

{
    const transactionTest = new InternalTransactionReshardingTest({reshardInPlace: false});
    transactionTest.runTestForInsertUpdateDeleteDuringResharding(
        transactionTest.InternalTxnType.kNonRetryable, true /* abortOnInitialTry */);
    transactionTest.stop();
}
})();
