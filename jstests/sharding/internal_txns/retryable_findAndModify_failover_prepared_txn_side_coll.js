/*
 * Tests that prepared retryable internal transactions for findAndModify on a shard with image
 * collection enabled are retryable across failover.
 *
 * @tags: [requires_fcv_60, uses_transactions, exclude_from_large_txns]
 */
import {RetryableInternalTransactionTest} from "jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js";

const transactionTest = new RetryableInternalTransactionTest(
    {} /*collectionOptions*/,
    true /*initiateWithDefaultElectionTimeout*/,
);
transactionTest.runTestsForAllPreparedRetryableInternalTransactionTypes(
    transactionTest.runFindAndModifyTestsEnableImageCollection,
    transactionTest.TestMode.kFailover,
);
transactionTest.stop();
