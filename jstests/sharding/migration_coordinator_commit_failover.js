/**
 * Tests that a donor resumes coordinating a migration if it fails over after creating the
 * migration coordinator document but before deleting it.
 */

// This test induces failovers on shards.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
'use strict';

load('jstests/sharding/migration_coordinator_failover_include.js');

const dbName = "test";

var st = new ShardingTest({shards: 2, rs: {nodes: 2}});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

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
})();
