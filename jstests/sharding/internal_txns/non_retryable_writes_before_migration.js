/**
 * Tests that a chunk migration does not transfer the history for non-retryable internal
 * transactions that commit or abort on the donor before the migration to the recipient.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence, exclude_from_large_txns]
 */
(function() {
"use strict";

load("jstests/sharding/internal_txns/libs/chunk_migration_test.js");

const transactionTest = new InternalTransactionChunkMigrationTest();
transactionTest.runTestForInsertUpdateDeleteBeforeChunkMigration(
    transactionTest.InternalTxnType.kNonRetryable, false /* abortOnInitialTry */);
transactionTest.runTestForInsertUpdateDeleteBeforeChunkMigration(
    transactionTest.InternalTxnType.kNonRetryable, true /* abortOnInitialTry */);
transactionTest.stop();
})();
