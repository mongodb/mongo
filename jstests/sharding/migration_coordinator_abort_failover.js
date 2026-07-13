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
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";

// Try and prevent split vote failed elections after freezing / unfreezing by preventing the
// secondary from being electable.
let st = new ShardingTest({
    shards: 2,
    rs: {nodes: [{rsConfig: {}}, {rsConfig: {priority: 0}}]},
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true,
});

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

const adminDB = st.s.getDB("admin");
const usesMoveRangeCoordinatorPath = FeatureFlagUtil.isPresentAndEnabled(
    adminDB,
    "AuthoritativeShardsDDL",
);
const expectedMigrationCommitFailureCodes = usesMoveRangeCoordinatorPath
    ? [ErrorCodes.ConflictingOperationInProgress]
    : [ErrorCodes.StaleEpoch];

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "moveChunkHangAtStep3",
    false /* shouldMakeMigrationFailToCommitOnConfig */,
);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "moveChunkHangAtStep4",
    false /* shouldMakeMigrationFailToCommitOnConfig */,
);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "moveChunkHangAtStep5",
    false /* shouldMakeMigrationFailToCommitOnConfig */,
);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    usesMoveRangeCoordinatorPath
        ? "hangInMoveRangeCoordinatorGlobalCatalogCommit"
        : "hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    expectedMigrationCommitFailureCodes,
);

if (usesMoveRangeCoordinatorPath) {
    runMoveChunkMakeDonorStepDownAfterFailpoint(
        st,
        dbName,
        "hangInMoveRangeCoordinatorShardCatalogCommit",
        false /* shouldMakeMigrationFailToCommitOnConfig */,
    );
}

if (!usesMoveRangeCoordinatorPath) {
    // This scenario forces the migration commit to fail after the donor has refreshed its filtering
    // metadata. The authoritative MoveRangeCoordinator rejects a disallowed commit at the earlier global
    // catalog commit phase (covered by the kGlobalCatalogCommit scenario above) and aborts before ever
    // reaching the shard catalog commit, so this scenario only applies to the legacy path.
    runMoveChunkMakeDonorStepDownAfterFailpoint(
        st,
        dbName,
        "hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible",
        true /* shouldMakeMigrationFailToCommitOnConfig */,
        expectedMigrationCommitFailureCodes,
    );
}

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    expectedMigrationCommitFailureCodes,
);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    expectedMigrationCommitFailureCodes,
);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    expectedMigrationCommitFailureCodes,
);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInAdvanceTxnNumThenSimulateErrorUninterruptible",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    expectedMigrationCommitFailureCodes,
);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangBeforeMakingAbortDecisionDurable",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    expectedMigrationCommitFailureCodes,
);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangBeforeSendingAbortDecision",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    expectedMigrationCommitFailureCodes,
);

runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangBeforeForgettingMigrationAfterAbortDecision",
    true /* shouldMakeMigrationFailToCommitOnConfig */,
    expectedMigrationCommitFailureCodes,
);

st.stop();
