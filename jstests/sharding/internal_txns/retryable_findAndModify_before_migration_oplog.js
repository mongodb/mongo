/**
 * Tests that retryable findAndModify statements that are executed with the image collection
 * disabled inside internal transactions that start and commit on the donor before a chunk migration
 * are retryable on the recipient after the migration.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence, exclude_from_large_txns]
 */
(function() {
"use strict";

load("jstests/sharding/internal_txns/libs/chunk_migration_test.js");

const transactionTest =
    new InternalTransactionChunkMigrationTest(false /* storeFindAndModifyImagesInSideCollection */);
transactionTest.runTestForFindAndModifyBeforeChunkMigration(
    transactionTest.InternalTxnType.kRetryable, false /* abortOnInitialTry */);
transactionTest.stop();
})();
