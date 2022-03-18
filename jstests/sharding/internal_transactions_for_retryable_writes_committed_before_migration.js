/**
 * Tests that retryable insert, update and delete statements that are executed inside internal
 * transactions that start and commit on the donor before a chunk migration are retryable on the
 * recipient after the migration.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/internal_transaction_chunk_migration_test.js");

const transactionTest = new InternalTransactionChunkMigrationTest();
transactionTest.runTestForInsertUpdateDeleteBeforeChunkMigration(
    transactionTest.InternalTxnType.kRetryable, false /* abortOnInitialTry */);
transactionTest.stop();
})();
