/**
 * Tests that a donor resumes coordinating a migration if it fails over after creating the
 * migration coordinator document but before deleting it.
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
var st = new ShardingTest({
    shards: 2,
    rs: {nodes: [{rsConfig: {}}, {rsConfig: {priority: 0}}]},
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true
});

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

runMoveChunkMakeDonorStepDownAfterFailpoint(st,
                                            dbName,
                                            "hangBeforeMakingCommitDecisionDurable",
                                            false /* shouldMakeMigrationFailToCommitOnConfig */);
runMoveChunkMakeDonorStepDownAfterFailpoint(st,
                                            dbName,
                                            "hangBeforeSendingCommitDecision",
                                            false /* shouldMakeMigrationFailToCommitOnConfig */);
runMoveChunkMakeDonorStepDownAfterFailpoint(st,
                                            dbName,
                                            "hangBeforeForgettingMigrationAfterCommitDecision",
                                            false /* shouldMakeMigrationFailToCommitOnConfig */);
runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible",
    false /* shouldMakeMigrationFailToCommitOnConfig */);
runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible",
    false /* shouldMakeMigrationFailToCommitOnConfig */);
runMoveChunkMakeDonorStepDownAfterFailpoint(
    st,
    dbName,
    "hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible",
    false /* shouldMakeMigrationFailToCommitOnConfig */);
runMoveChunkMakeDonorStepDownAfterFailpoint(st,
                                            dbName,
                                            "hangInAdvanceTxnNumThenSimulateErrorUninterruptible",
                                            false /* shouldMakeMigrationFailToCommitOnConfig */);

st.stop();
