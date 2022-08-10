/**
 * Tests that resharding does not transfer the history for non-retryable internal transactions that
 * commit or abort on the donor(s) before resharding to the recipient.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence, exclude_from_large_txns]
 */
(function() {
"use strict";

load("jstests/sharding/internal_txns/libs/resharding_test.js");

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
