/**
 * Kills the OperationContext used by the donor shard to send
 * _configsvrEnsureChunkVersionIsGreaterThan and to force a filtering metadata refresh.
 *
 * Depends on the checkOrphansAreDeleted hook at the end of ShardingTest to verify that the orphans,
 * range deletion tasks, and migration coordinator state are deleted despite the killOps.
 *
 * Legacy migration-path test exercising RecoverRefreshThread-based recovery. Safe to remove once
 * featureFlagAuthoritativeShardsDDL is always enabled.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
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

let st = new ShardingTest({shards: 2});

const donorShard = st.shard0;
const recipientShard = st.shard1;
const numDocs = 1000;
const middle = numDocs / 2;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: donorShard.shardName}),
);

const usesAuthoritativePath = FeatureFlagUtil.isPresentAndEnabled(
    st.s.getDB("admin"),
    "AuthoritativeShardsDDL",
);

function testKillOpAfterFailPoint(failPointName, getCurrentOpMatch) {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log("Testing with " + tojson(arguments) + " using ns " + ns);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: middle}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: donorShard.shardName}),
    );
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: middle}, to: donorShard.shardName}),
    );

    // Insert some docs into the collection.
    let bulk = st.s.getDB(dbName).getCollection(collName).initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    // Simulate a network error on sending commit to the config server, so that the donor tries to
    // recover the commit decision.
    let commitFailpoint;
    if (!usesAuthoritativePath) {
        commitFailpoint = configureFailPoint(donorShard, "migrationCommitNetworkError");
    }

    // Set the requested failpoint and launch the moveChunk asynchronously.
    let failPoint = configureFailPoint(donorShard, failPointName);
    const awaitResult = startParallelShell(
        funWithArgs(
            function (ns, toShardName, middle, usesAuthoritativePath) {
                // When authoritative shards DDL is enabled, processManualMigrationOutcome
                // faithfully returns the error from _shardsvrMoveRange rather than
                // reconstructing the commit decision by reading the routing table. Reading
                // the routing table is not legal when shards own authoritative metadata, as
                // the config server may be in a partial-commit state. Unlike legacy, where
                // killOp targets RecoverRefreshThread, authoritative killOp must target the
                // blocked _shardsvrMoveRange command itself, which interrupts that attempt.
                // Recovery is verified separately below via doc counts.
                if (usesAuthoritativePath) {
                    db.adminCommand({moveChunk: ns, find: {_id: middle}, to: toShardName});
                } else {
                    assert.commandWorked(
                        db.adminCommand({moveChunk: ns, find: {_id: middle}, to: toShardName}),
                    );
                }
            },
            ns,
            recipientShard.shardName,
            middle,
            usesAuthoritativePath,
        ),
        st.s.port,
    );

    jsTest.log("Waiting for moveChunk to reach " + failPointName + " failpoint");
    failPoint.wait();

    let matchingOps;
    assert.soon(() => {
        matchingOps = donorShard
            .getDB("admin")
            .aggregate([
                {$currentOp: {"allUsers": true, "idleConnections": true}},
                {$match: getCurrentOpMatch(ns)},
            ])
            .toArray();
        // Wait for the opid to be present, since it's possible for currentOp to run after the
        // Client has been created but before it has been associated with a new
        // OperationContext.
        return 1 === matchingOps.length && matchingOps[0].opid != null;
    }, "Failed to find op to kill");
    donorShard.getDB("admin").killOp(matchingOps[0].opid);

    failPoint.off();

    awaitResult();

    // Allow the moveChunk to finish:
    if (commitFailpoint) {
        commitFailpoint.off();
    }
    jsTest.log("Make sure the recovery is executed");
    assert.soon(function () {
        try {
            return st.s0.getDB(dbName).getCollection(collName).countDocuments({}) == numDocs;
        } catch (e) {
            if (e.code == ErrorCodes.Interrupted) {
                // Expected as the request may have joined the filtering metadata refresh that
                // the killOp above interrupted.
                return false;
            }
            throw e;
        }
    });
}

const recoverRefreshThreadMatch = (_) => ({desc: {$regex: "RecoverRefreshThread"}});
const shardsvrMoveRangeMatch = (ns) => ({"command._shardsvrMoveRange": ns});

if (usesAuthoritativePath) {
    testKillOpAfterFailPoint(
        "hangInMoveRangeCoordinatorGlobalCatalogCommit",
        shardsvrMoveRangeMatch,
    );
    testKillOpAfterFailPoint(
        "hangInMoveRangeCoordinatorShardCatalogCommit",
        shardsvrMoveRangeMatch,
    );
} else {
    // TODO(SERVER-127253): Remove legacy branch.
    testKillOpAfterFailPoint(
        "hangInEnsureChunkVersionIsGreaterThanInterruptible",
        recoverRefreshThreadMatch,
    );
    testKillOpAfterFailPoint(
        "hangInPersistMigrateCommitDecisionInterruptible",
        recoverRefreshThreadMatch,
    );
    testKillOpAfterFailPoint(
        "hangInDeleteRangeDeletionOnRecipientInterruptible",
        recoverRefreshThreadMatch,
    );
    testKillOpAfterFailPoint(
        "hangInReadyRangeDeletionLocallyInterruptible",
        recoverRefreshThreadMatch,
    );
    testKillOpAfterFailPoint("hangInAdvanceTxnNumInterruptible", recoverRefreshThreadMatch);
}

st.stop();
