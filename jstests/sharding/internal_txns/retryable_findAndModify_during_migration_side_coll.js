/**
 * Tests that retryable findAndModify statements that are executed with the image collection
 * enabled inside internal transactions that start and commit on the donor during a chunk migration
 * are retryable on the recipient after the migration.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence, exclude_from_large_txns]
 */
import {InternalTransactionChunkMigrationTest} from "jstests/sharding/internal_txns/libs/chunk_migration_test.js";

const transactionTest = new InternalTransactionChunkMigrationTest();

// TODO (SERVER-124153): Remove the failpoint.
const isMultiversion =
    Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion);
if (!isMultiversion) {
    for (let i = 0; i < 3; i++) {
        assert.commandWorked(
            transactionTest.st[`rs${i}`]
                .getPrimary()
                .adminCommand({configureFailPoint: "useInMemoryReplicatedSizeCount", mode: "alwaysOn"}),
        );
    }
}

transactionTest.runTestForFindAndModifyDuringChunkMigration(
    transactionTest.InternalTxnType.kRetryable,
    false /* abortOnInitialTry */,
);
transactionTest.stop();
