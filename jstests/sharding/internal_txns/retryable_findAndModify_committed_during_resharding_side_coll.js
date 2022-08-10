/**
 * Tests that retryable findAndModify statements that are executed with image collection enabled
 * inside internal transactions that start and commit the donor(s) during resharding are retryable
 * on the recipient after resharding.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence, exclude_from_large_txns]
 */
(function() {
"use strict";

load("jstests/sharding/internal_txns/libs/resharding_test.js");

const storeFindAndModifyImagesInSideCollection = true;
const abortOnInitialTry = false;

{
    const transactionTest = new InternalTransactionReshardingTest(
        {reshardInPlace: false, storeFindAndModifyImagesInSideCollection});
    transactionTest.runTestForFindAndModifyDuringResharding(
        transactionTest.InternalTxnType.kRetryable, abortOnInitialTry);
    transactionTest.stop();
}

{
    const transactionTest = new InternalTransactionReshardingTest(
        {reshardInPlace: true, storeFindAndModifyImagesInSideCollection});
    transactionTest.runTestForFindAndModifyDuringResharding(
        transactionTest.InternalTxnType.kRetryable, abortOnInitialTry);
    transactionTest.stop();
}
})();
