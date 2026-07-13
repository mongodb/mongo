/**
 * Shuts down the donor primary at two points in the critical section: while the node is executing
 * _configsvrEnsureChunkVersionIsGreaterThan and while the node is forcing a filtering metadata
 * refresh.
 *
 * Shuts down a donor shard which leads mongos to retry if the donor is also the config server, and
 * this can fail waiting for read preference if the shard is slow to recover.
 * @tags: [
 *   does_not_support_stepdowns,
 *   # Require persistence to restart nodes
 *   requires_persistence,
 *   config_shard_incompatible,
 * ]
 */

// This test shuts down a shard primary.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function getNewNs(dbName) {
    if (typeof getNewNs.counter == "undefined") {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

const dbName = "test";
const st = new ShardingTest({shards: 2});

const donorShard = st.shard0;
const recipientShard = st.shard1;
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: donorShard.shardName}),
);

const usesAuthoritativePath = FeatureFlagUtil.isPresentAndEnabled(
    st.s.getDB("admin"),
    "AuthoritativeShardsDDL",
);

function testShutDownAfterFailPoint(failPointName) {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log("Testing with " + tojson(arguments) + " using ns " + ns);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Insert some docs into the collection.
    const numDocs = 1000;
    let bulk = st.s.getDB(dbName).getCollection(collName).initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    // Simulate a network error on sending commit to the config server, so that the donor tries to
    // recover the commit decision.
    if (!usesAuthoritativePath) {
        configureFailPoint(donorShard.rs.getPrimary(), "migrationCommitNetworkError");
    }

    // Set the requested failpoint and launch the moveChunk asynchronously.
    let failPoint = configureFailPoint(donorShard.rs.getPrimary(), failPointName);
    const awaitResult = startParallelShell(
        funWithArgs(
            function (ns, toShardName, usesAuthoritativePath) {
                // When authoritative shards DDL is enabled, processManualMigrationOutcome
                // faithfully returns the error from _shardsvrMoveRange rather than
                // reconstructing the commit decision by reading the routing table. Reading
                // the routing table is not legal when shards own authoritative metadata, as
                // the config server may be in a partial-commit state. Depending on how far
                // recovery has progressed when the donor is shut down, moveChunk may succeed
                // or fail, so we do not assert on its outcome.
                if (usesAuthoritativePath) {
                    db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName});
                } else {
                    assert.commandWorked(
                        db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}),
                    );
                }
            },
            ns,
            recipientShard.shardName,
            usesAuthoritativePath,
        ),
        st.s.port,
    );

    jsTest.log("Waiting for moveChunk to reach " + failPointName + " failpoint");
    failPoint.wait();

    let primary = donorShard.rs.getPrimary();
    let primary_id = donorShard.rs.getNodeId(primary);

    // Ensure we are able to shut down the donor primary by asserting that its exit code is 0.
    assert.eq(0, donorShard.rs.stop(primary_id, null, {}, {forRestart: true, waitPid: true}));

    // Restart the donor before waiting for the moveChunk result. When AuthoritativeShardsDDL is
    // enabled, processManualMigrationOutcome returns the error from _shardsvrMoveRange faithfully
    // rather than reconstructing the commit decision from the config server's routing table. The
    // moveChunk retry loop can only make progress once the donor's MoveRangeCoordinator completes
    // its recovery, which requires the donor to be running. Awaiting the result before restarting
    // would deadlock: awaitResult() blocks until moveChunk returns, but moveChunk cannot return
    // until the donor is up. Restarting first is safe for the legacy path as well.
    donorShard.rs.start(primary_id, {}, true /* restart */, true /* waitForHealth */);
    awaitResult();
}

if (usesAuthoritativePath) {
    testShutDownAfterFailPoint("hangInMoveRangeCoordinatorGlobalCatalogCommit");
    testShutDownAfterFailPoint("hangInMoveRangeCoordinatorShardCatalogCommit");
} else {
    // TODO(SERVER-127253): Remove legacy branch.
    testShutDownAfterFailPoint("hangInEnsureChunkVersionIsGreaterThanInterruptible");
    testShutDownAfterFailPoint("hangInRefreshFilteringMetadataUntilSuccessInterruptible");
    testShutDownAfterFailPoint("hangInPersistMigrateCommitDecisionInterruptible");
    testShutDownAfterFailPoint("hangInDeleteRangeDeletionOnRecipientInterruptible");
    testShutDownAfterFailPoint("hangInReadyRangeDeletionLocallyInterruptible");
    testShutDownAfterFailPoint("hangInAdvanceTxnNumInterruptible");
}

st.stop();
