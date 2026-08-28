/*
 * Verifies that a MultiUpdateCoordinator releases migrations even when the FCV changes while it is
 * mid-flight.
 *
 * MultiUpdateCoordinator blocks migrations in its kBlockMigrations phase and unblocks them later in
 * `_stopBlockingMigrationsIfNeeded`. The migration-blocking work runs through either the V1
 * (MigrationBlockingOperationCoordinator) or V2 (MigrationBlockingOperationCoordinatorV2)
 * coordinator, selected from the authoritative-shards DDL feature (`featureFlagAuthoritativeShardsDDL`,
 * FCV-gated). A single operation must block and unblock through the same coordinator version;
 * otherwise the unblock could target a coordinator other than the one holding migrations blocked and
 * migrations would never be released. This test blocks migrations, changes the FCV while the
 * coordinator is paused mid-flight, and asserts migrations are still released.
 *
 * @tags: [
 *   requires_fcv_90,
 *   # This test drives the cluster FCV between lastLTSFCV and latestFCV. Those constants come from
 *   # the shell's binary and are not consistent across nodes in a multiversion cluster, so there is
 *   # no FCV the whole cluster agrees on for this test to transition to.
 *   multiversion_incompatible,
 * ]
 */

// lastLTSFCV and latestFCV are injected as shell globals; no import needed.
import {migrationsAreAllowed} from "jstests/libs/chunk_manipulation_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const dbName = "test";
const collName = "migration_blocking_operation_fcv_transition";
const namespace = `${dbName}.${collName}`;

// Starts a multi-update through mongos in a parallel shell (it blocks at the
// hangAfterBlockingMigrations failpoint). With pauseMigrationsDuringMultiUpdates enabled the update
// routes through the MultiUpdateCoordinator on the shard, which blocks migrations for its duration.
// Returns the parallel-shell join function.
function runParallelUpdate(st) {
    return startParallelShell(
        funWithArgs(
            function (dbNameArg, collNameArg) {
                assert.commandWorked(
                    db
                        .getSiblingDB(dbNameArg)
                        [
                            collNameArg
                        ].update({member: "abc123"}, {$set: {points: 50}}, {multi: true}),
                );
            },
            dbName,
            collName,
        ),
        st.s.port,
    );
}

// Enables pauseMigrationsDuringMultiUpdates and waits for every mongos to observe it, so multi
// updates are routed through the MultiUpdateCoordinator.
function enablePauseMigrations(st) {
    assert.commandWorked(
        st.s.adminCommand({
            setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: true}},
        }),
    );
    st.forEachMongos((mongos) => {
        assert.soon(() => {
            const response = assert.commandWorked(
                mongos.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}),
            );
            return response.clusterParameters[0].enabled;
        });
    });
}

describe("MultiUpdateCoordinator migration blocking across FCV transitions", function () {
    before(function () {
        this.st = new ShardingTest({shards: {rs0: {nodes: 1}}});
        this.db = this.st.s.getDB(dbName);
        const coll = this.st.s.getCollection(namespace);
        CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {key: 1}, [
            {min: {key: MinKey}, max: {key: MaxKey}, shard: this.st.shard0.shardName},
        ]);
        assert.commandWorked(
            coll.insertMany([
                {key: 1, member: "abc123", points: 0},
                {key: 2, member: "abc123", points: 59},
            ]),
        );
        this.shardPrimary = this.st.rs0.getPrimary();
        enablePauseMigrations(this.st);
    });

    after(function () {
        this.st.stop();
    });

    function setFCV(conn, fcv) {
        // setFeatureCompatibilityVersion performs index DDL on config collections (e.g. dropping or
        // creating the FCV-gated indexes on config.shards). In continuous config-stepdown suites the
        // command is interrupted and retried, but the index build kicked off by the interrupted
        // attempt survives the stepdown, so the retry can transiently observe it and fail with
        // BackgroundOperationInProgressForNamespace. Retry until the build completes.
        retryOnRetryableError(
            () =>
                assert.commandWorked(
                    conn.adminCommand({setFeatureCompatibilityVersion: fcv, confirm: true}),
                ),
            10 /* numRetries */,
            1000 /* sleepMs */,
            [ErrorCodes.BackgroundOperationInProgressForNamespace],
        );
    }

    afterEach(function () {
        setFCV(this.st.s, latestFCV);
    });

    // Blocks migrations at `startFCV`, changes FCV to `endFCV` while the coordinator is paused
    // between blocking and unblocking, then verifies the coordinator finishes and migrations are
    // released.
    function runTransitionCase(ctx, startFCV, endFCV) {
        const st = ctx.st;
        const shardPrimary = ctx.shardPrimary;

        setFCV(st.s, startFCV);
        assert(migrationsAreAllowed(ctx.db, collName));

        // Pause the coordinator after it has blocked migrations but before it unblocks them.
        const fp = configureFailPoint(shardPrimary, "hangAfterBlockingMigrations");

        const updateShell = runParallelUpdate(st);

        fp.wait();
        assert(
            !migrationsAreAllowed(ctx.db, collName),
            "migrations should be blocked while paused",
        );

        // Change the FCV while the coordinator is paused mid-flight, then let it proceed to its
        // unblock phase. The coordinator version is pinned to the operation's persisted metadata, so
        // it unblocks through the same version that blocked migrations despite the FCV change.
        const fcvShell = startParallelShell(
            funWithArgs(
                function (setFCVFn, fcv) {
                    setFCVFn(db, fcv);
                },
                setFCV,
                endFCV,
            ),
            st.s.port,
        );

        // Wait until the shard observes the FCV transition before releasing the coordinator, so the
        // unblock happens under the changed FCV rather than racing it. setFCV parks in the
        // transitional state while the coordinator holds the DDL lock, so targetVersion stays set
        // for us to observe.
        assert.soon(() => {
            const fcv = assert.commandWorked(
                shardPrimary.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}),
            ).featureCompatibilityVersion;
            return fcv.targetVersion !== undefined;
        }, "shard never observed the FCV transition starting");
        fp.off();

        // The coordinator must unblock migrations regardless of the FCV change. Assert this with a
        // bounded timeout before joining the shells, so the assertion (not a shell join) is what
        // fails if migrations are left blocked.
        assert.soon(
            () => migrationsAreAllowed(ctx.db, collName),
            "migrations were never unblocked after the FCV transition",
            60 * 1000,
        );

        // Both the coordinator and the FCV change should then complete.
        updateShell();
        fcvShell();
    }

    it("releases migrations after upgrading FCV mid-flight", function () {
        runTransitionCase(this, lastLTSFCV, latestFCV);
    });

    it("releases migrations after downgrading FCV mid-flight", function () {
        runTransitionCase(this, latestFCV, lastLTSFCV);
    });
});
