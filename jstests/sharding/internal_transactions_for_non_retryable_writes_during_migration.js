/**
 * Tests that a chunk migration does not transfer the history for non-retryable internal
 * transactions that commit or abort on the donor during the migration to the recipient.
 *
 * TODO (SERVER-63877): Determine if chunk migration should migrate internal sessions for
 * non-retryable writes.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/internal_transaction_chunk_migration_test.js");

const transactionTest = new InternalTransactionChunkMigrationTest();
transactionTest.runTestForInsertUpdateDeleteDuringChunkMigration(
    transactionTest.InternalTxnType.kNonRetryable, false /* abortOnInitialTry */);
transactionTest.runTestForInsertUpdateDeleteDuringChunkMigration(
    transactionTest.InternalTxnType.kNonRetryable, true /* abortOnInitialTry */);
transactionTest.stop();
})();
