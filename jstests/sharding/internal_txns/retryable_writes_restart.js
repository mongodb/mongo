/*
 * Tests that retryable internal transactions for insert, update and delete are retryable across
 * restart.
 *
 * Exclude this test from large_txn variants because the variant enforces that the max transaction
 * oplog entry length is 2 operations, and the oplog length assertions in this test do not account
 * for this. We are not losing test coverage as this test inherently tests large transactions.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence, exclude_from_large_txns]
 */
import {RetryableInternalTransactionTest} from "jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js";

const transactionTest = new RetryableInternalTransactionTest(
    {} /*collectionOptions*/,
    true /*initiateWithDefaultElectionTimeout*/,
);
transactionTest.runTestsForAllRetryableInternalTransactionTypes(
    transactionTest.runInsertUpdateDeleteTests,
    transactionTest.TestMode.kRestart,
);
transactionTest.stop();
