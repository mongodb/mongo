/**
 * Tests how movePrimary behaves when its destination shard starts draining after the coordinator
 * has already persisted a phase and is then interrupted and recovered.
 *
 * The MovePrimaryCoordinator re-evaluates its preconditions on every execution attempt. Re-checking
 * "destination shard is draining" used to fail with a non-retryable ShardNotFound that the
 * coordinator nevertheless retried indefinitely, because it must always make forward progress once
 * a phase has been persisted. That deadlocked shard removal: removeShard waits for all DDL
 * coordinators on the participant shards to quiesce, so the drain could never finish, so the
 * destination shard stayed in draining mode, so the coordinator could never succeed.
 *
 * The commit phase is now the point of no return. Before it, a draining destination aborts the
 * coordinator: the data cloned onto the recipient is dropped and the database primary stays put.
 * From the commit phase onwards the database primary may already have moved, so the preconditions
 * are no longer re-checked and the coordinator rolls forward through the remaining phases instead,
 * dropping the donor's stale data and releasing the critical section. Either way the operation
 * terminates and leaves the catalogs consistent.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   assumes_balancer_off,
 *   requires_fcv_90,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kCoordinatorsColl = "system.sharding_ddl_coordinators";
const expectedDocs = [{x: 1}, {x: 2}, {x: 3}];

/**
 * Returns the number of persisted movePrimary coordinator documents on the donor shard. The primary
 * is resolved on every call because these tests deliberately step it down.
 */
function countMovePrimaryCoordinators(st) {
    return st.rs0
        .getPrimary()
        .getDB("config")
        .getCollection(kCoordinatorsColl)
        .find({"_id.operationType": "movePrimary"})
        .itcount();
}

/**
 * Steps the donor's current primary down, forcing the in-progress coordinator to be interrupted and
 * recovered on another node.
 */
function forceCoordinatorRecovery(st) {
    jsTest.log.info("Stepping down the donor primary to force coordinator recovery");
    st.rs0.stepUp(st.rs0.getSecondaries()[0], {awaitReplicationBeforeStepUp: false});
}

function setUpDatabase(st, dbName, collNs, primaryShardName) {
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}),
    );
    assert.commandWorked(st.s.getCollection(collNs).insert(expectedDocs));
}

describe("movePrimary interrupted before commit with a draining destination", function () {
    let st;
    let donorShard;
    let recipientShard;
    const dbName = `${jsTestName()}_pre_commit`;
    const untrackedCollNs = `${dbName}.untrackedColl`;

    before(function () {
        st = new ShardingTest({shards: 2, rs: {nodes: 3}});
        donorShard = st.shard0;
        recipientShard = st.shard1;
        setUpDatabase(st, dbName, untrackedCollNs, donorShard.shardName);
    });

    after(function () {
        st.stop();
    });

    it("aborts the coordinator", function () {
        // Pause the coordinator inside the clone phase. By this point the phase has been persisted,
        // so a subsequent execution is no longer the first one.
        const fp = configureFailPoint(st.rs0.getPrimary(), "hangBeforeCloningData");

        const awaitMovePrimary = startParallelShell(
            funWithArgs(
                function (dbName, toShard) {
                    // The recovered coordinator re-checks its preconditions, sees the draining
                    // destination and aborts. A fresh coordinator created by mongos' own retry
                    // rejects it up front for the same reason, so either interleaving surfaces
                    // ShardNotFound; what matters is that the command returns at all.
                    assert.commandFailedWithCode(
                        db.adminCommand({movePrimary: dbName, to: toShard}),
                        ErrorCodes.ShardNotFound,
                    );
                },
                dbName,
                recipientShard.shardName,
            ),
            st.s.port,
        );

        jsTest.log.info("Waiting for movePrimary to reach the clone phase");
        fp.wait();

        jsTest.log.info("Marking the destination shard as draining while movePrimary is paused");
        assert.commandWorked(st.s.adminCommand({startShardDraining: recipientShard.shardName}));

        forceCoordinatorRecovery(st);
        fp.off();

        // The regression this guards against: the coordinator stays on disk forever, retrying
        // ShardNotFound roughly twice a second, and the movePrimary command never returns. This is
        // asserted before joining the parallel shell so that a regression fails here within the
        // timeout, rather than blocking indefinitely on a command that will never complete.
        assert.soonRetryOnNetworkErrors(
            () => countMovePrimaryCoordinators(st) === 0,
            "movePrimary coordinator was never cleaned up, so it is still retrying",
            60 * 1000,
        );

        awaitMovePrimary();

        // The database primary must not have moved to the draining shard.
        assert.eq(
            st.s.getDB("config").databases.findOne({_id: dbName}).primary,
            donorShard.shardName,
        );
        assert.sameMembers(
            st.s.getCollection(untrackedCollNs).find({}, {_id: 0}).toArray(),
            expectedDocs,
        );

        // Aborting must leave nothing behind on the recipient either.
        const inconsistencies = st.s.getDB(dbName).checkMetadataConsistency().toArray();
        assert.eq(0, inconsistencies.length, "unexpected metadata inconsistencies", {
            inconsistencies,
        });

        assert.commandWorked(st.s.adminCommand({stopShardDraining: recipientShard.shardName}));
    });
});

describe("movePrimary interrupted after commit with a draining destination", function () {
    let st;
    let donorShard;
    let recipientShard;
    const dbName = `${jsTestName()}_post_commit`;
    const untrackedCollName = "untrackedColl";
    const untrackedCollNs = `${dbName}.${untrackedCollName}`;

    before(function () {
        st = new ShardingTest({shards: 2, rs: {nodes: 3}});
        donorShard = st.shard0;
        recipientShard = st.shard1;
        setUpDatabase(st, dbName, untrackedCollNs, donorShard.shardName);
    });

    after(function () {
        st.stop();
    });

    it("rolls forward through the remaining phases", function () {
        // Suspend the coordinator on entry to the clean phase, which is persisted before the
        // handler runs. At that point the database primary has already been committed to the
        // recipient, so the only work left is dropping the donor's now-stale collections and
        // releasing the critical section.
        const fp = configureFailPoint(st.rs0.getPrimary(), "suspendDDLCoordinatorOnPhase", {
            operationType: "movePrimary",
            phase: "clean",
        });

        const awaitMovePrimary = startParallelShell(
            funWithArgs(
                function (dbName, toShard) {
                    // Past the commit phase the recovered coordinator no longer re-checks its
                    // preconditions, so the draining destination does not stop it. The donor
                    // stepdown below makes mongos retry the command, which either joins the
                    // recovered coordinator or, once the routing table reports the recipient as the
                    // primary, short-circuits as a no-op. Both report success.
                    assert.commandWorked(db.adminCommand({movePrimary: dbName, to: toShard}));
                },
                dbName,
                recipientShard.shardName,
            ),
            st.s.port,
        );

        jsTest.log.info("Waiting for movePrimary to commit and reach the clean phase");
        fp.wait();

        // The commit has happened, so the catalog already reports the recipient as the primary.
        assert.eq(
            st.s.getDB("config").databases.findOne({_id: dbName}).primary,
            recipientShard.shardName,
        );

        jsTest.log.info("Marking the destination shard as draining while movePrimary is paused");
        assert.commandWorked(st.s.adminCommand({startShardDraining: recipientShard.shardName}));

        forceCoordinatorRecovery(st);
        fp.off();

        // The coordinator document is only removed after every phase has run, so this also
        // pins down that the recovered coordinator neither aborted nor kept retrying forever.
        assert.soonRetryOnNetworkErrors(
            () => countMovePrimaryCoordinators(st) === 0,
            "movePrimary coordinator was never cleaned up",
            60 * 1000,
        );

        awaitMovePrimary();

        assert.eq(
            st.s.getDB("config").databases.findOne({_id: dbName}).primary,
            recipientShard.shardName,
        );
        assert.sameMembers(
            st.s.getCollection(untrackedCollNs).find({}, {_id: 0}).toArray(),
            expectedDocs,
        );

        // The clean phase must have run: a stale copy left behind on the donor is what
        // checkMetadataConsistency reports as MisplacedCollection.
        assert(
            !st.rs0.getPrimary().getDB(dbName).getCollectionNames().includes(untrackedCollName),
            "expected the recovered coordinator to drop the stale data on the donor",
        );

        const inconsistencies = st.s.getDB(dbName).checkMetadataConsistency().toArray();
        assert.eq(0, inconsistencies.length, "unexpected metadata inconsistencies", {
            inconsistencies,
        });

        assert.commandWorked(st.s.adminCommand({stopShardDraining: recipientShard.shardName}));
    });
});
