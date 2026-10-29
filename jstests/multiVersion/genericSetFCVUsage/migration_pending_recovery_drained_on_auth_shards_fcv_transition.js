/**
 * Verify that a FCV upgrade across Authoritative Shards drains legacy migrations left pending
 * recovery (rather than relying on the asynchronous step-up recovery, which may race with setFCV).
 * TODO (SERVER-98118): Remove this test.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {moveChunkParallel} from "jstests/libs/chunk_manipulation_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: {rs0: {nodes: 2}, rs1: {nodes: 1}},
    // Disable the index consistency checker to ensure that the config server can't cause the
    // shards to refresh their filtering metadata, which would trigger the migration recovery.
    other: {configOptions: {setParameter: {enableShardedIndexConsistencyCheck: false}}},
});

// Downgrade the FCV so that we run the migration in legacy mode
assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);
if (FeatureFlagUtil.isPresentAndEnabled(st.s.getDB("admin"), "AuthoritativeShardsDDL")) {
    jsTest.log.info("Skipping: featureFlagAuthoritativeShardsDDL already enabled at lastLTSFCV");
    st.stop();
    quit();
}

// Set up a sharded collection
const dbName = "test";
const ns = dbName + ".coll";

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Leave a legacy migration pending recovery, blocking the step-up recovery before it can refresh,
// so that only setFCV can drain the migration.
const staticMongod = MongoRunner.runMongod({});
const hangAtCommit = configureFailPoint(st.rs0.getPrimary(), "moveChunkHangAtStep5");
const joinMove = moveChunkParallel(
    staticMongod,
    st.s0.host,
    {_id: 0},
    null,
    ns,
    st.shard1.shardName,
    true /* expectSuccess */,
);
hangAtCommit.wait();

const commitNetworkError = configureFailPoint(st.rs0.getPrimary(), "migrationCommitNetworkError");
const skipRefresh = configureFailPoint(st.rs0.getPrimary(), "skipShardFilteringMetadataRefresh");

const secondary = st.rs0.getSecondary();
const blockStepUpRecoveryFP = configureFailPoint(secondary, "hangBeforeFilteringMetadataRefresh");

hangAtCommit.off();
commitNetworkError.wait();
st.rs0.stepUp(secondary);
blockStepUpRecoveryFP.wait();

joinMove();
commitNetworkError.off();
skipRefresh.off();

assert.eq(1, st.rs0.getPrimary().getDB("config").migrationCoordinators.count());

// Step-up recovery is frozen, so only the FCV upgrade drain can clear the migration.
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
assert.eq(0, st.rs0.getPrimary().getDB("config").migrationCoordinators.count());

blockStepUpRecoveryFP.off();
MongoRunner.stopMongod(staticMongod);
st.stop();
