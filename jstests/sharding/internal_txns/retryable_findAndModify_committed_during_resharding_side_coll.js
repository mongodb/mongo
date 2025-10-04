/**
 * Tests that retryable findAndModify statements that are executed with image collection enabled
 * inside internal transactions that start and commit the donor(s) during resharding are retryable
 * on the recipient after resharding.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence, exclude_from_large_txns]
 */
import {InternalTransactionReshardingTest} from "jstests/sharding/internal_txns/libs/resharding_test.js";

const abortOnInitialTry = false;

{
    const transactionTest = new InternalTransactionReshardingTest({reshardInPlace: false});
    transactionTest.runTestForFindAndModifyDuringResharding(
        transactionTest.InternalTxnType.kRetryable,
        abortOnInitialTry,
    );
    transactionTest.stop();
}

{
    const transactionTest = new InternalTransactionReshardingTest({reshardInPlace: true});
    transactionTest.runTestForFindAndModifyDuringResharding(
        transactionTest.InternalTxnType.kRetryable,
        abortOnInitialTry,
    );
    transactionTest.stop();
}
