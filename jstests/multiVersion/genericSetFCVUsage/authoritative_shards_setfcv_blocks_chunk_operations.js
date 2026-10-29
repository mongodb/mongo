/**
 * Validates that chunk operations cannot start while setFCV is transitioning authoritative shard
 * metadata. The block is driven by the persisted transitional FCV, so it must survive a shard
 * primary failover and only lift once the FCV reaches a stable state.
 *
 * The suite covers, for the full matrix of chunk operations (split, mergeChunks,
 * mergeAllChunksOnShard, moveChunk, moveRange):
 *   1. Manual chunk operations are rejected while a downgrade/upgrade is mid-transition.
 *   2. The balancer cannot move chunks while setFCV is in progress.
 *   3. The block persists across a shard primary stepdown (it is not in-memory state).
 *   4. An operation that started before the transition cannot commit its placement change
 *      once the FCV has gone transitional.
 *   5. setFCV drains an in-flight authoritative chunk operation that has created its coordinator
 *      but has not yet registered in the ActiveMigrationsRegistry.
 *   6. setFCV drains an in-flight chunk command that has decided its authoritativeness but not
 *      yet registered in the ActiveMigrationsRegistry nor created a coordinator.
 *
 * TODO (SERVER-98118): Remove this test.
 */

import {configureFailPoint, configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

// A parallel setFCV can be interrupted by a stepdown; tolerate the resulting transient errors.
const kSetFCVTransientErrorCodes = [
    ErrorCodes.InterruptedDueToReplStateChange,
    ErrorCodes.Interrupted,
    ErrorCodes.NotWritablePrimary,
    ErrorCodes.ShutdownInProgress,
];

describe("setFCV blocks chunk operations for authoritative shards", function () {
    let st;
    let testCounter = 0;
    const dbName = "auth_shards_setfcv_chunk_ops";

    // ---- Cluster/state helpers -------------------------------------------------------------

    function uniqueCollName(prefix) {
        return `${prefix}_${testCounter++}`;
    }

    function countChunks(ns, query = {}) {
        return findChunksUtil.countChunksForNs(st.s.getDB("config"), ns, query);
    }

    function assertAuthoritativeShardFlags({enabled}) {
        const adminDB = st.s.getDB("admin");
        assert.eq(
            enabled,
            FeatureFlagUtil.isPresentAndEnabled(adminDB, "AuthoritativeShardsDDL"),
            `AuthoritativeShardsDDL flag should be ${enabled}`,
        );
        assert.eq(
            enabled,
            FeatureFlagUtil.isPresentAndEnabled(adminDB, "AuthoritativeShardsCRUD"),
            `AuthoritativeShardsCRUD flag should be ${enabled}`,
        );
    }

    // Drives the FCV to a stable state and asserts the flags follow it: enabled at latestFCV,
    // disabled at lastLTSFCV.
    function setStableFCV(targetFCV) {
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}),
        );
        assertAuthoritativeShardFlags({enabled: targetFCV === latestFCV});
    }

    function setupShardedCollection(prefix, splitPoints = []) {
        const collName = uniqueCollName(prefix);
        const ns = `${dbName}.${collName}`;
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
        splitPoints.forEach((splitPoint) => {
            assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: splitPoint}}));
        });
        return {collName, ns};
    }

    function assertAllChunksOnShard(ns, shardName, expectedCount) {
        assert.eq(expectedCount, countChunks(ns), `${ns}: unexpected total chunk count`);
        assert.eq(
            expectedCount,
            countChunks(ns, {shard: shardName}),
            `${ns}: expected all chunks on ${shardName}`,
        );
    }

    // ---- Chunk operation matrix ------------------------------------------------------------
    //
    // Each spec knows how to set itself up, issue its command, assert success, and (for the
    // migration family) assert that no placement change committed. 'preCommitFailPoint' is the
    // donor-side failpoint that pauses the operation right before it commits to the config server.

    function operationSpecs() {
        const assertPlacementUnchanged = ({ns}) =>
            assert.eq(
                0,
                countChunks(ns, {shard: st.shard1.shardName}),
                `${ns}: placement changed during the FCV transition`,
            );
        return [
            {
                name: "split",
                setup: () => setupShardedCollection("split"),
                command: ({ns}) => ({split: ns, middle: {x: 0}}),
                assertWorked: ({ns}) =>
                    assert.eq(2, countChunks(ns), `${ns}: split did not commit`),
            },
            {
                name: "mergeChunks",
                setup: () => setupShardedCollection("merge", [0]),
                command: ({ns}) => ({mergeChunks: ns, bounds: [{x: MinKey}, {x: MaxKey}]}),
                assertWorked: ({ns}) =>
                    assert.eq(1, countChunks(ns), `${ns}: mergeChunks did not commit`),
            },
            {
                name: "mergeAllChunksOnShard",
                setup: () => setupShardedCollection("merge_all", [0, 10]),
                command: ({ns}) => ({mergeAllChunksOnShard: ns, shard: st.shard0.shardName}),
                assertWorked: ({ns}) =>
                    assert.eq(1, countChunks(ns), `${ns}: mergeAllChunksOnShard did not commit`),
            },
            {
                name: "moveChunk",
                setup: () => ({
                    ...setupShardedCollection("move_chunk", [0]),
                    toShard: st.shard1.shardName,
                }),
                command: ({ns, toShard}) => ({
                    moveChunk: ns,
                    find: {x: 0},
                    to: toShard,
                    _waitForDelete: true,
                }),
                assertWorked: ({ns}) =>
                    assert.eq(
                        1,
                        countChunks(ns, {shard: st.shard1.shardName}),
                        `${ns}: moveChunk did not commit`,
                    ),
                // Pauses the donor in the migration critical section right before it commits the new
                // placement to the config server, which is what makes the commit-race observable.
                preCommitFailPoint: "moveChunkHangAtStep5",
                assertNotCommitted: assertPlacementUnchanged,
            },
            {
                name: "moveRange",
                setup: () => ({
                    ...setupShardedCollection("move_range", [0]),
                    toShard: st.shard1.shardName,
                }),
                command: ({ns, toShard}) => ({
                    moveRange: ns,
                    min: {x: 0},
                    max: {x: MaxKey},
                    toShard,
                }),
                assertWorked: ({ns}) =>
                    assert.eq(
                        1,
                        countChunks(ns, {shard: st.shard1.shardName}),
                        `${ns}: moveRange did not commit`,
                    ),
                preCommitFailPoint: "moveChunkHangAtStep5",
                assertNotCommitted: assertPlacementUnchanged,
            },
        ];
    }

    // Sets up a fresh collection for every operation and returns [{spec, context}] so the same
    // collections can later be exercised (e.g. expected blocked, then expected to work).
    function prepareOperations() {
        return operationSpecs().map((spec) => ({spec, context: spec.setup()}));
    }

    function runOperationsExpectWorked(label, prepared = prepareOperations()) {
        prepared.forEach(({spec, context}) => {
            jsTest.log.info(`Verifying ${spec.name} succeeds while FCV is stable`, {label});
            assert.commandWorked(st.s.adminCommand(spec.command(context)));
            spec.assertWorked(context);
        });
    }

    function runOperationsExpectBlocked(label, prepared, {retry = false} = {}) {
        prepared.forEach(({spec, context}) => {
            jsTest.log.info(`Verifying ${spec.name} is blocked during setFCV`, {label});
            if (!retry) {
                assert.commandFailedWithCode(
                    st.s.adminCommand(spec.command(context)),
                    ErrorCodes.ConflictingOperationInProgress,
                );
                return;
            }
            // Retry to tolerate transient failover errors until mongos routes to the new primary; a
            // success would mean the block is broken.
            assert.soon(() => {
                const res = st.s.adminCommand(spec.command(context));
                if (res.code === ErrorCodes.ConflictingOperationInProgress) {
                    return true;
                }
                assert.commandFailed(
                    res,
                    `${spec.name} must not succeed while the FCV is transitional (${label})`,
                );
                jsTest.log.info(`${spec.name} returned a retryable error; retrying`, {label, res});
                return false;
            }, `${spec.name} must stay blocked while the FCV is transitional (${label})`);
        });
    }

    // ---- setFCV orchestration helpers ------------------------------------------------------

    // Waits until 'conn' reports a transitional FCV (one that carries a targetVersion).
    function awaitTransitionalFCV(conn) {
        assert.soon(() => {
            const fcv = assert.commandWorked(
                conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}),
            ).featureCompatibilityVersion;
            return fcv.targetVersion !== undefined;
        }, "node did not reach the transitional FCV");
    }

    // Runs setFCV from a parallel shell against mongos. When 'expectSuccess' is false the shell
    // tolerates the transient errors a concurrent stepdown can produce.
    function startSetFCVInParallel(targetFCV, {expectSuccess}) {
        return startParallelShell(
            funWithArgs(
                function (targetFCV, expectSuccess, transientCodes) {
                    const res = db.adminCommand({
                        setFeatureCompatibilityVersion: targetFCV,
                        confirm: true,
                    });
                    if (expectSuccess) {
                        assert.commandWorked(res);
                    } else {
                        assert.commandWorkedOrFailedWithCode(res, transientCodes);
                    }
                },
                targetFCV,
                expectSuccess,
                kSetFCVTransientErrorCodes,
            ),
            st.s.port,
        );
    }

    // Starts a setFCV that hangs at 'failPointName' on the shard primary, returning a handle that
    // pauses the transition in its transitional state until released.
    function startPausedSetFCV(targetFCV, failPointName, {expectSuccess = true} = {}) {
        const fp = configureFailPoint(st.rs0.getPrimary(), failPointName);
        const awaitSetFCV = startSetFCVInParallel(targetFCV, {expectSuccess});
        fp.wait();
        return {fp, awaitSetFCV};
    }

    function finishPausedSetFCV({fp, awaitSetFCV}) {
        fp.off();
        awaitSetFCV();
    }

    // Pauses a setFCV mid-transition, runs 'body' while it is parked, then always releases it.
    function withPausedSetFCV(targetFCV, failPointName, body) {
        const paused = startPausedSetFCV(targetFCV, failPointName);
        try {
            body();
        } finally {
            finishPausedSetFCV(paused);
        }
    }

    // ---- Fixture lifecycle -----------------------------------------------------------------

    before(function () {
        st = new ShardingTest({
            name: "authoritative_shards_setfcv_blocks_chunk_operations_main",
            shards: 2,
            rs: {nodes: 3},
            other: {chunkSize: 1, enableBalancer: false},
        });

        // The test needs the flags off at lastLTS (only so while last LTS is 8.0) and on at latest;
        // skip otherwise.
        if (
            lastLTSFCV !== "8.0" ||
            !FeatureFlagUtil.isEnabled(st.s.getDB("admin"), "AuthoritativeShardsCRUD")
        ) {
            st.stop();
            quit();
        }

        [st.configRS.nodes, st.rs0.nodes, st.rs1.nodes].forEach((nodes) => {
            configureFailPointForRS(
                nodes,
                "overrideHistoryWindowInSecs",
                {seconds: -10},
                "alwaysOn",
            );
        });
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
    });

    after(function () {
        if (st) {
            st.stop();
        }
    });

    beforeEach(function () {
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
    });

    afterEach(function () {
        assert.commandWorked(st.stopBalancer());
        setStableFCV(latestFCV);

        const inconsistencies = st.s.getDB("admin").checkMetadataConsistency().toArray();
        assert.eq(0, inconsistencies.length, "Metadata inconsistencies found", {inconsistencies});

        assert.commandWorked(st.s.getDB(dbName).dropDatabase());
    });

    // ---- Test cases ------------------------------------------------------------------------

    // Every manual chunk operation must be rejected while a downgrade and (separately) an upgrade
    // are paused mid-transition, and must work again once the FCV settles.
    it("rejects manual chunk operations during downgrade and upgrade transitions", function () {
        setStableFCV(latestFCV);
        runOperationsExpectWorked("before setFCV");

        const downgradeOperations = prepareOperations();
        withPausedSetFCV(lastLTSFCV, "hangWhileDowngrading", () => {
            runOperationsExpectBlocked("downgrading", downgradeOperations);
        });
        assertAuthoritativeShardFlags({enabled: false});

        const upgradeOperations = prepareOperations();
        withPausedSetFCV(latestFCV, "hangWhileUpgrading", () => {
            runOperationsExpectBlocked("upgrading", upgradeOperations);
        });
        assertAuthoritativeShardFlags({enabled: true});

        runOperationsExpectWorked("after setFCV");
    });

    // While a downgrade is paused mid-transition the balancer must not be able to move any chunk;
    // once the FCV settles it can move chunks again.
    it("does not let the balancer move chunks while setFCV is in progress", function () {
        setStableFCV(latestFCV);
        const {collName, ns} = setupShardedCollection("balancer", [0, 10, 20, 30]);
        const bigString = "x".repeat(1024 * 1024);
        assert.commandWorked(
            st.s
                .getDB(dbName)
                .getCollection(collName)
                .insert([
                    {x: -10, padding: bigString},
                    {x: 5, padding: bigString},
                    {x: 15, padding: bigString},
                    {x: 25, padding: bigString},
                ]),
        );
        assertAllChunksOnShard(ns, st.shard0.shardName, 5);

        withPausedSetFCV(lastLTSFCV, "hangWhileDowngrading", () => {
            assert.commandWorked(st.startBalancer());
            st.awaitBalancerRound();
            assert.commandWorked(st.stopBalancer());
            assertAllChunksOnShard(ns, st.shard0.shardName, 5);
        });
        assertAuthoritativeShardFlags({enabled: false});

        setStableFCV(latestFCV);
        assert.commandWorked(st.startBalancer());
        for (let i = 0; i < 10 && countChunks(ns, {shard: st.shard1.shardName}) === 0; i++) {
            st.awaitBalancerRound();
        }
        assert.gt(
            countChunks(ns, {shard: st.shard1.shardName}),
            0,
            `${ns}: balancer did not move any chunks after setFCV completed`,
        );
        assert.commandWorked(st.stopBalancer());
    });

    // The block is driven by the persisted transitional FCV, with no in-memory state, so it must
    // keep rejecting chunk operations across a donor-shard primary stepdown.
    it("keeps blocking chunk operations across a shard primary stepdown", function () {
        setStableFCV(latestFCV);

        // Prepare every chunk operation in the matrix while the FCV is still stable.
        const operations = prepareOperations();

        // Pause the downgrade on *every* donor-shard node, so the FCV cannot leave the transitional
        // state across a failover: whichever node is primary will hang in _runDowngrade.
        const downgradeFp = configureFailPointForRS(st.rs0.nodes, "hangWhileDowngrading");
        const awaitDowngrade = startSetFCVInParallel(lastLTSFCV, {expectSuccess: false});

        try {
            // The parallel setFCV must reach the paused downgrade (transitional FCV) before we
            // exercise the block.
            awaitTransitionalFCV(st.rs0.getPrimary());
            runOperationsExpectBlocked("before stepdown", operations, {retry: true});

            // Force a donor-shard failover mid-transition. The new primary inherits the persisted
            // transitional FCV (and re-hangs in _runDowngrade), so the block must persist.
            const oldPrimary = st.rs0.getPrimary();
            st.rs0.stepUp(st.rs0.getSecondary(), {awaitReplicationBeforeStepUp: false});
            assert.neq(oldPrimary.host, st.rs0.getPrimary().host);

            awaitTransitionalFCV(st.rs0.getPrimary());
            runOperationsExpectBlocked("after stepdown", operations, {retry: true});
        } finally {
            // Clearing the failpoint lets the transition run to completion.
            downgradeFp.off();
            awaitDowngrade();
        }

        // Once the FCV settles back at a stable, fully-upgraded state every operation is unblocked.
        setStableFCV(lastLTSFCV);
        setStableFCV(latestFCV);
        runOperationsExpectWorked("after stepdown transition", operations);
    });

    // An operation that started and cloned while the FCV was stable must not be allowed to commit
    // its placement change once the transition has begun. Only the migration family has a
    // pre-commit failpoint to make this race observable; the other operations exercise the same
    // config-server commit gate in the "rejects manual chunk operations ..." test, where their
    // commit happens while the config server FCV is already transitional.
    it("blocks chunk operations from committing a placement change during the FCV transition", function () {
        operationSpecs()
            .filter((spec) => spec.preCommitFailPoint)
            .forEach((spec) => {
                jsTest.log.info(
                    `Verifying ${spec.name} cannot commit a placement change mid-transition`,
                );
                setStableFCV(latestFCV);

                const context = spec.setup();

                // Pause the operation on the donor right before it commits the new placement to the
                // config server. It therefore starts and clones while the FCV is still stable, and
                // only reaches the config-server commit once the transition is already in progress.
                const hangBeforeCommit = configureFailPoint(
                    st.rs0.getPrimary(),
                    spec.preCommitFailPoint,
                );
                const awaitOperation = startParallelShell(
                    funWithArgs(
                        function (commandFn, context) {
                            assert.commandFailedWithCode(
                                db.adminCommand(commandFn(context)),
                                ErrorCodes.ConflictingOperationInProgress,
                            );
                        },
                        spec.command,
                        context,
                    ),
                    st.s.port,
                );
                hangBeforeCommit.wait();

                // Begin a downgrade. The config server flips its own (authoritative) FCV to the
                // transitional state first, before rolling kStart out to the shards.
                const awaitDowngrade = startSetFCVInParallel(lastLTSFCV, {expectSuccess: false});

                try {
                    // Wait until the config server has entered the transitional FCV (its rollout to
                    // the shards is now parked behind the still-in-flight operation's drain).
                    awaitTransitionalFCV(st.configRS.getPrimary());

                    // Release the operation so it attempts to commit while the transition is ongoing.
                    hangBeforeCommit.off();
                    awaitOperation();

                    // The placement must be unchanged: the operation was not allowed to commit.
                    spec.assertNotCommitted(context);
                } finally {
                    hangBeforeCommit.off();
                    awaitDowngrade();
                }

                // Drive the FCV back to a stable, fully-upgraded state before the next operation.
                setStableFCV(latestFCV);
            });
    });

    it("drains an in-flight chunk op coordinator that has not yet registered the migration", function () {
        setStableFCV(latestFCV);
        const {ns} = setupShardedCollection("drain_chunk_coordinator");

        // Pause the coordinator after it registers with the sharding coordinator service but before
        // it runs, so only the coordinator drain (not the ActiveMigrationsRegistry) can observe it.
        const hangSplit = configureFailPoint(
            st.rs0.getPrimary(),
            "PrimaryOnlyServiceHangBeforeRunningInstance",
        );
        const splitThread = new Thread(
            (mongosHost, ns) => {
                return new Mongo(mongosHost).getDB("admin").runCommand({split: ns, middle: {x: 0}});
            },
            st.s.host,
            ns,
        );
        splitThread.start();
        hangSplit.wait();

        // setFCV drains the still-paused coordinator, so we expect a timeout.
        assert.commandFailedWithCode(
            st.s.adminCommand({
                setFeatureCompatibilityVersion: lastLTSFCV,
                confirm: true,
                maxTimeMS: 5000,
            }),
            ErrorCodes.MaxTimeMSExpired,
        );
        assert(!isFCVEqual(st.rs0.getPrimary().getDB("admin"), lastLTSFCV));

        // Releasing the coordinator lets it finish; the split may commit or conflict depending on
        // how far setFCV reached, either is fine.
        hangSplit.off();
        splitThread.join();
        assert.commandWorkedOrFailedWithCode(
            splitThread.returnData(),
            ErrorCodes.ConflictingOperationInProgress,
        );

        // Finish the interrupted downgrade now that nothing is draining, leaving a stable FCV.
        setStableFCV(lastLTSFCV);
    });

    it("drains an in-flight legacy migration before reaching the transitional FCV", function () {
        setStableFCV(lastLTSFCV);
        const {ns} = setupShardedCollection("drain_chunk_command");

        // Pause the legacy moveRange right before it registers in the ActiveMigrationsRegistry.
        const hangMoveRange = configureFailPoint(
            st.rs0.getPrimary(),
            "hangBeforeLegacyRegisterDonateChunk",
        );
        const moveRangeThread = new Thread(
            (mongosHost, ns, toShard) => {
                return new Mongo(mongosHost).getDB("admin").runCommand({
                    moveRange: ns,
                    min: {x: MinKey},
                    toShard,
                });
            },
            st.s.host,
            ns,
            st.shard1.shardName,
        );
        moveRangeThread.start();
        hangMoveRange.wait();

        // setFCV drains it, so we expect a timeout.
        assert.commandFailedWithCode(
            st.s.adminCommand({
                setFeatureCompatibilityVersion: latestFCV,
                confirm: true,
                maxTimeMS: 3000,
            }),
            ErrorCodes.MaxTimeMSExpired,
        );
        checkFCV(st.rs0.getPrimary().getDB("admin"), lastLTSFCV);

        // Releasing the coordinator lets it finish; the split may commit or conflict depending on
        // how far setFCV reached, either is fine.
        hangMoveRange.off();
        moveRangeThread.join();
        assert.commandWorkedOrFailedWithCode(
            moveRangeThread.returnData(),
            ErrorCodes.ConflictingOperationInProgress,
        );

        // Finish the interrupted upgrade now that nothing is draining, leaving a stable FCV.
        setStableFCV(latestFCV);
    });
});
