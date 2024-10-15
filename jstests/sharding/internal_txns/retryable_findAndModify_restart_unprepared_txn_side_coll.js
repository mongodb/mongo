/*
 * Tests that unprepared retryable internal transactions for findAndModify on a shard with image
 * collection enabled are retryable across restart.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence, exclude_from_large_txns]
 */
import {
    RetryableInternalTransactionTest
} from "jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js";

const transactionTest = new RetryableInternalTransactionTest(
    {} /*collectionOptions*/, true /*initiateWithDefaultElectionTimeout*/);
transactionTest.runTestsForAllUnpreparedRetryableInternalTransactionTypes(
    transactionTest.runFindAndModifyTestsEnableImageCollection, transactionTest.TestMode.kRestart);
transactionTest.stop();