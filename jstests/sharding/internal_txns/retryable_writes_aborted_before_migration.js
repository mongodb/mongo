/**
 * Tests that retryable insert, update and delete statements that are executed inside internal
 * transactions that start and abort on the donor before a chunk migration are retryable on the
 * recipient after the migration.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence, exclude_from_large_txns]
 */
import {InternalTransactionChunkMigrationTest} from "jstests/sharding/internal_txns/libs/chunk_migration_test.js";

const transactionTest = new InternalTransactionChunkMigrationTest();
transactionTest.runTestForInsertUpdateDeleteBeforeChunkMigration(
    transactionTest.InternalTxnType.kRetryable,
    true /* abortOnInitialTry */,
);
transactionTest.stop();
