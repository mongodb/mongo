/**
 * Tests that retryable insert, update and delete statements that are executed inside internal
 * transactions that start and abort on the donor before a chunk migration are retryable on the
 * recipient after the migration.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/internal_txns/libs/chunk_migration_test.js");

const transactionTest = new InternalTransactionChunkMigrationTest();
transactionTest.runTestForInsertUpdateDeleteBeforeChunkMigration(
    transactionTest.InternalTxnType.kRetryable, true /* abortOnInitialTry */);
transactionTest.stop();
})();
