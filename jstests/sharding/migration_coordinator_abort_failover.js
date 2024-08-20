/**
 * Tests that a donor resumes coordinating a migration if it fails over after creating the
 * migration coordinator document but before deleting it.
 *
 * @tags [requires_fcv_71]
 */

// This test induces failovers on shards.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

import {
    runMoveChunkMakeDonorStepDownAfterFailpoint
} from "jstests/sharding/migration_coordinator_failover_include.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";

// Try and prevent split vote failed elections after freezing / unfreezing by preventing the
// secondary from being electable.
var st = new ShardingTest({shards: 2, rs: {nodes: [{rsConfig: {}}, {rsConfig: {priority: 0}}]}});

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st, dbName, "moveChunkHangAtStep3", false /* shouldMakeMigrationFailToCommitOnConfig */);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st, dbName, "moveChunkHangAtStep4", false /* shouldMakeMigrationFailToCommitOnConfig */);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st, dbName, "moveChunkHangAtStep5", false /* shouldMakeMigrationFailToCommitOnConfig */);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    [ErrorCodes.StaleEpoch]);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    [ErrorCodes.StaleEpoch]);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    [ErrorCodes.StaleEpoch]);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    [ErrorCodes.StaleEpoch]);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    [ErrorCodes.StaleEpoch]);

runMoveChunkMakeDonorStepDownAfterFailpoint(st,
                                            dbName,
                                            "hangInAdvanceTxnNumThenSimulateErrorUninterruptible",
                                            true /* shouldMakeMigrationFailToCommitOnConfig */,
                                            [ErrorCodes.StaleEpoch]);

runMoveChunkMakeDonorStepDownAfterFailpoint(st,
                                            dbName,
                                            "hangBeforeMakingAbortDecisionDurable",
                                            true /* shouldMakeMigrationFailToCommitOnConfig */,
                                            [ErrorCodes.StaleEpoch]);

runMoveChunkMakeDonorStepDownAfterFailpoint(st,
                                            dbName,
                                            "hangBeforeSendingAbortDecision",
                                            true /* shouldMakeMigrationFailToCommitOnConfig */,
                                            [ErrorCodes.StaleEpoch]);

runMoveChunkMakeDonorStepDownAfterFailpoint(st,
                                            dbName,
                                            "hangBeforeForgettingMigrationAfterAbortDecision",
                                            true /* shouldMakeMigrationFailToCommitOnConfig */,
                                            [ErrorCodes.StaleEpoch]);

st.stop();
