/**
 * Tests change stream v2 behavior across FCV upgrade/downgrade transitions.
 * Verifies no events are lost and cursor targeting changes when the shard targeter
 * switches between v1 and v2.
 * TODO (SERVER-98118): Remove once featureFlagChangeStreamPreciseShardTargeting reaches last-lts.
 *
 * @tags: [
 *   requires_sharding,
 *   uses_change_streams,
 *   featureFlagChangeStreamPreciseShardTargeting,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it, before, beforeEach, afterEach, after} from "jstests/libs/mochalite.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    ChangeStreamTest,
    ChangeStreamWatchMode,
    assertOpenCursors,
    cursorCommentFilter,
    ensureShardDistribution,
    getClusterTime,
    awaitLogMessageCodes,
    V2TargeterLogCodes,
    watchModeToString,
} from "jstests/libs/query/change_stream_util.js";
import {
    InsertDocCommand,
    FCVDowngradeCommand,
    FCVUpgradeCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

/**
 * Creates an InsertDocCommand for the test collection.
 * The command auto-generates a unique _id and knows which change events it produces.
 */
function makeInsertCmd(dbName, collName) {
    return new InsertDocCommand(dbName, collName, /* shardSet */ null, /* collectionCtx */ {exists: true});
}

/**
 * Executes an array of commands and asserts the change stream delivers the expected events.
 */
function executeAndAssertEvents({cst, cursor, conn, cmds, watchMode}) {
    for (const cmd of cmds) {
        cmd.execute(conn);
    }

    const expectedChanges = cmds.flatMap((cmd) => cmd.getChangeEvents(watchMode));

    if (expectedChanges.length > 0) {
        cst.assertNextChangesEqual({cursor, expectedChanges});
    }
}

describe("change stream v2", function () {
    let st;
    let conn;
    let cst;

    const dbName = jsTestName();
    const collName = "test";

    // Shard names for cursor assertions (initialized in before()).
    let allShardNames;

    // Only the 2 shards that hold collection data (shard0 + shard1).
    let dataShardNames;

    before(function () {
        st = new ShardingTest({
            shards: 3,
            mongos: 1,
            config: 1,
            configShard: false,
            rs: {
                nodes: 1,
                setParameter: {
                    writePeriodicNoops: true,
                    periodicNoopIntervalSecs: 1,
                },
            },
            mongosOptions: {
                setParameter: {
                    logComponentVerbosity: tojson({query: {verbosity: 3}}),
                },
            },
            configOptions: {
                setParameter: {
                    writePeriodicNoops: true,
                    periodicNoopIntervalSecs: 1,
                },
            },
            other: {enableBalancer: false},
        });
        conn = st.s;

        allShardNames = [st.shard0.shardName, st.shard1.shardName, st.shard2.shardName];
        dataShardNames = [st.shard0.shardName, st.shard1.shardName];
    });

    beforeEach(function () {
        // Ensure FCV is at latestFCV at the start of each test.
        new FCVUpgradeCommand().execute(conn);

        // Clear the mongos log so awaitLogMessageCodes offsets work correctly.
        assert.commandWorked(conn.adminCommand({clearLog: "global"}));
    });

    afterEach(function () {
        cst.cleanUp();

        // Re-add shard1 if it was removed by an IRS degraded mode test.
        const shards = st.s
            .getDB("config")
            .shards.find()
            .toArray()
            .map((s) => s._id);
        if (!shards.includes(st.shard1.shardName)) {
            // Drop the local database on shard1 before re-adding. addShard rejects shards that
            // have a database already present elsewhere.
            assert.commandWorked(st.rs1.getPrimary().getDB(dbName).dropDatabase());
            assert.commandWorked(st.s.adminCommand({addShard: st.rs1.getURL(), name: st.shard1.shardName}));
        }

        assert.commandWorked(st.s.getDB(dbName).dropDatabase());
    });

    after(function () {
        st.stop();
    });

    /**
     * Creates and shards a collection across shard0 and shard1 (2 of 3 shards).
     */
    function setupShardedCollection() {
        const db = st.s.getDB(dbName);

        // Enable sharding on the test database with shard0 as primary.
        assert.commandWorked(db.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

        assertCreateCollection(db, collName);

        assert.commandWorked(db.adminCommand({shardCollection: `${dbName}.${collName}`, key: {_id: 1}}));

        db[collName].insertMany([{_id: -1}, {_id: 1}]);
        ensureShardDistribution(db, db[collName], {
            middle: {_id: 0},
            chunks: [
                {find: {_id: -1}, to: st.shard0.shardName},
                {find: {_id: 1}, to: st.shard1.shardName},
            ],
            expectedCounts: [
                [st.shard0, 1],
                [st.shard1, 1],
                [st.shard2, 0],
            ],
        });
    }

    /**
     * Opens a change stream with the given parameters.
     */
    function openChangeStream(cst, {watchMode, comment, ...spec}) {
        if (watchMode === ChangeStreamWatchMode.kCluster) {
            spec.allChangesForCluster = true;
        }

        const collection = watchMode === ChangeStreamWatchMode.kCollection ? collName : 1;

        return cst.startWatchingChanges({
            pipeline: [{$changeStream: spec}],
            collection: collection,
            aggregateOptions: {cursor: {batchSize: 0}, comment: comment},
        });
    }

    const version = "v2";

    describe("FCV downgrade", function () {
        describe("DbPresent state", function () {
            for (const watchMode of [ChangeStreamWatchMode.kCollection, ChangeStreamWatchMode.kDb]) {
                const scope = watchModeToString(watchMode);

                it(`${scope}-scope: stream transparently falls back to v1 on FCV downgrade`, function () {
                    setupShardedCollection();

                    const db = conn.getDB(dbName);
                    const comment = "db_present_downgrade";
                    cst = new ChangeStreamTest(db);

                    // Opening the stream at latestFCV initializes placement in strict mode.
                    const cursor = openChangeStream(cst, {watchMode, version, comment});
                    awaitLogMessageCodes(conn, [V2TargeterLogCodes.kCollOrDbShardTargeterInitStrictMode], () => {
                        cst.assertNoChange(cursor);
                    });
                    const v2CursorId = cursor.id;

                    // v2 targets only data-bearing shards (shard0 + shard1).
                    assertOpenCursors(
                        st,
                        dataShardNames,
                        /* expectedConfigCursor */ false,
                        cursorCommentFilter(comment),
                    );

                    // FCV downgrade.
                    new FCVDowngradeCommand().execute(conn);

                    // The getMore processes the NPC event from FCV downgrade. The v2 targeter
                    // detects placement is no longer available and transitions to Downgrading.
                    awaitLogMessageCodes(
                        conn,
                        [
                            V2TargeterLogCodes.kShardTargeterDbPresentPlacementNotAvailableSwitchToV1,
                            V2TargeterLogCodes.kTopologyHandlerStageStateTransition,
                        ],
                        () => {
                            cst.assertNoChange(cursor);
                        },
                    );

                    // The next getMore detects Downgrading and throws RetryChangeStream, which reopens the cursor as v1.
                    executeAndAssertEvents({cst, cursor, conn, watchMode, cmds: [makeInsertCmd(dbName, collName)]});

                    // The stream was reopened as v1 after RetryChangeStream, so the cursor ID must differ.
                    const v1CursorId = cursor.id;
                    assert.neq(v1CursorId, v2CursorId, "cursor should have been reopened after FCV downgrade");

                    // After FCV downgrade, v1 broadcasts to all shards including the config server.
                    assertOpenCursors(st, allShardNames, /* expectedConfigCursor */ true, cursorCommentFilter(comment));
                });
            }
        });

        describe("DbAbsent state", function () {
            for (const watchMode of [ChangeStreamWatchMode.kCollection, ChangeStreamWatchMode.kDb]) {
                const scope = watchModeToString(watchMode);

                it(`${scope}-scope: stream transparently falls back to v1 on FCV downgrade`, function () {
                    const db = st.s.getDB(dbName);
                    const comment = "db_absent_downgrade";
                    cst = new ChangeStreamTest(db);

                    // Opening the stream at latestFCV initializes placement in strict mode.
                    const cursor = openChangeStream(cst, {watchMode, version, comment});
                    awaitLogMessageCodes(conn, [V2TargeterLogCodes.kCollOrDbShardTargeterInitStrictMode], () => {
                        cst.assertNoChange(cursor);
                    });
                    const v2CursorId = cursor.id;

                    // v2 targets only configsvr as database does not exist.
                    assertOpenCursors(st, [], /* expectedConfigCursor */ true, cursorCommentFilter(comment));

                    // FCV downgrade.
                    new FCVDowngradeCommand().execute(conn);

                    // The getMore processes the NPC event from FCV downgrade. The v2 targeter
                    // detects placement is no longer available and transitions to Downgrading.
                    awaitLogMessageCodes(
                        conn,
                        [
                            V2TargeterLogCodes.kShardTargeterDbAbsentPlacementNotAvailableSwitchToV1,
                            V2TargeterLogCodes.kTopologyHandlerStageStateTransition,
                        ],
                        () => {
                            cst.assertNoChange(cursor);
                        },
                    );

                    // The next getMore detects Downgrading and throws RetryChangeStream, which reopens the cursor as v1.
                    executeAndAssertEvents({cst, cursor, conn, watchMode, cmds: [makeInsertCmd(dbName, collName)]});

                    // The stream was reopened as v1 after RetryChangeStream, so the cursor ID must differ.
                    const v1CursorId = cursor.id;
                    assert.neq(v1CursorId, v2CursorId, "cursor should have been reopened after FCV downgrade");

                    // After downgrade, v1 broadcasts to all shards including the config server.
                    assertOpenCursors(st, allShardNames, /* expectedConfigCursor */ true, cursorCommentFilter(comment));
                });
            }
        });

        it("cluster-scope: stream transparently falls back to v1 on FCV downgrade", function () {
            const watchMode = ChangeStreamWatchMode.kCluster;

            setupShardedCollection();

            const adminDB = st.s.getDB("admin");
            const comment = "cluster_scope_downgrade";
            cst = new ChangeStreamTest(adminDB);

            // Opening the stream at latestFCV initializes placement in strict mode.
            const cursor = openChangeStream(cst, {watchMode, version, comment});
            awaitLogMessageCodes(conn, [V2TargeterLogCodes.kClusterShardTargeterInitStrictMode], () => {
                cst.assertNoChange(cursor);
            });
            const v2CursorId = cursor.id;

            // Cluster-scope v2 targets only data-bearing shards + configsvr.
            assertOpenCursors(st, dataShardNames, /* expectedConfigCursor */ true, cursorCommentFilter(comment));

            // FCV downgrade.
            new FCVDowngradeCommand().execute(conn);

            // The getMore processes the NPC event from FCV downgrade. The v2 targeter
            // detects placement is no longer available and transitions to Downgrading.
            awaitLogMessageCodes(
                conn,
                [
                    V2TargeterLogCodes.kClusterShardTargeterPlacementNotAvailableSwitchToV1,
                    V2TargeterLogCodes.kTopologyHandlerStageStateTransition,
                ],
                () => {
                    cst.assertNoChange(cursor);
                },
            );

            // The next getMore detects Downgrading and throws RetryChangeStream, which reopens the cursor as v1.
            executeAndAssertEvents({cst, cursor, conn, watchMode, cmds: [makeInsertCmd(dbName, collName)]});

            // The stream was reopened as v1 after RetryChangeStream, so the cursor ID must differ.
            const v1CursorId = cursor.id;
            assert.neq(v1CursorId, v2CursorId, "cursor should have been reopened after FCV downgrade");

            // After FCV downgrade, v1 broadcasts to all shards including the config server.
            assertOpenCursors(st, allShardNames, /* expectedConfigCursor */ true, cursorCommentFilter(comment));

            cst.assertNoChange(cursor);
        });

        for (const watchMode of [
            ChangeStreamWatchMode.kCollection,
            ChangeStreamWatchMode.kDb,
            ChangeStreamWatchMode.kCluster,
        ]) {
            const scope = watchModeToString(watchMode);

            it(`${scope}-scope: resume token from v2 stream remains valid after downgrade`, function () {
                setupShardedCollection();

                const isCluster = watchMode === ChangeStreamWatchMode.kCluster;
                const db = isCluster ? conn.getDB("admin") : conn.getDB(dbName);
                const comment = `resume_token_downgrade_${scope}`;

                cst = new ChangeStreamTest(db);

                // Open v2 stream and generate an event to capture a resume token.
                let resumeToken;
                {
                    const cursor = openChangeStream(cst, {watchMode, version, comment});

                    executeAndAssertEvents({cst, cursor, conn, watchMode, cmds: [makeInsertCmd(dbName, collName)]});

                    // v2 targets data-bearing shards; cluster scope also opens a config cursor.
                    assertOpenCursors(
                        st,
                        dataShardNames,
                        /* expectedConfigCursor */ isCluster,
                        cursorCommentFilter(comment),
                    );

                    resumeToken = cst.getResumeToken(cursor);
                }

                // Downgrade FCV.
                new FCVDowngradeCommand().execute(conn);

                // Reopen with the v2 resume token - should work as v1.
                {
                    const resumedComment = comment + "_resumed";
                    const cursor = openChangeStream(cst, {
                        watchMode,
                        resumeAfter: resumeToken,
                        comment: resumedComment,
                    });

                    executeAndAssertEvents({cst, cursor, conn, watchMode, cmds: [makeInsertCmd(dbName, collName)]});

                    // Resumed stream runs as v1 - broadcasts to all shards including the config server.
                    assertOpenCursors(
                        st,
                        allShardNames,
                        /* expectedConfigCursor */ true,
                        cursorCommentFilter(resumedComment),
                    );

                    cst.assertNoChange(cursor);
                }
            });
        }

        describe("ignoreRemovedShards", function () {
            for (const watchMode of [
                ChangeStreamWatchMode.kCollection,
                ChangeStreamWatchMode.kDb,
                ChangeStreamWatchMode.kCluster,
            ]) {
                const scope = watchModeToString(watchMode);
                const isCluster = watchMode === ChangeStreamWatchMode.kCluster;

                it(`${scope}-scope: IRS stream falls back to v1 on FCV downgrade`, function () {
                    setupShardedCollection();

                    const comment = `irs_nondegrade_downgrade_${scope}`;
                    const db = isCluster ? conn.getDB("admin") : conn.getDB(dbName);
                    cst = new ChangeStreamTest(db);

                    const startAtOperationTime = getClusterTime(db);
                    const initCode = isCluster
                        ? V2TargeterLogCodes.kClusterShardTargeterStartChangeStreamSegment
                        : V2TargeterLogCodes.kCollOrDbShardTargeterStartChangeStreamSegment;
                    const cursor = openChangeStream(cst, {
                        watchMode,
                        version,
                        ignoreRemovedShards: true,
                        startAtOperationTime,
                        comment,
                    });
                    awaitLogMessageCodes(conn, [initCode], () => {
                        cst.assertNoChange(cursor);
                    });
                    const v2CursorId = cursor.id;

                    // IRS in strict mode targets data-bearing shards; cluster also has configsvr.
                    assertOpenCursors(
                        st,
                        dataShardNames,
                        /* expectedConfigCursor */ isCluster,
                        cursorCommentFilter(comment),
                    );

                    // FCV downgrade.
                    new FCVDowngradeCommand().execute(conn);

                    // The v2 targeter detects placement is no longer available
                    // and transitions to Downgrading.
                    const switchCode = isCluster
                        ? V2TargeterLogCodes.kClusterShardTargeterPlacementNotAvailableSwitchToV1
                        : V2TargeterLogCodes.kShardTargeterDbPresentPlacementNotAvailableSwitchToV1;
                    awaitLogMessageCodes(
                        conn,
                        [switchCode, V2TargeterLogCodes.kTopologyHandlerStageStateTransition],
                        () => {
                            cst.assertNoChange(cursor);
                        },
                    );

                    // RetryChangeStream reopens the cursor as v1.
                    executeAndAssertEvents({
                        cst,
                        cursor,
                        conn,
                        watchMode,
                        cmds: [makeInsertCmd(dbName, collName)],
                    });

                    // The stream was reopened as v1 after RetryChangeStream, so the cursor ID must differ.
                    const v1CursorId = cursor.id;
                    assert.neq(v1CursorId, v2CursorId, "cursor should have been reopened after FCV downgrade");

                    // v1 broadcasts to all shards + config.
                    assertOpenCursors(st, allShardNames, /* expectedConfigCursor */ true, cursorCommentFilter(comment));
                });

                it(`${scope}-scope: IRS stream in degraded mode reacts to FCV downgrade`, function () {
                    const withBalancer = (fn) => {
                        assert.commandWorked(conn.adminCommand({balancerStart: 1}));
                        try {
                            fn();
                        } finally {
                            assert.commandWorked(conn.adminCommand({balancerStop: 1}));
                        }
                    };

                    const db = isCluster ? conn.getDB("admin") : conn.getDB(dbName);

                    // Use a sharded collection so placement history has moveChunk entries that create bounded degraded segments.
                    setupShardedCollection();

                    // Capture startAtOperationTime before any mutations.
                    const startAtOperationTime = getClusterTime(db);

                    // Remove shard1 with balancer. The balancer drains chunks (moveChunk) creating placement history entries.
                    // These produce bounded segments when the stream replays them.
                    withBalancer(() => {
                        removeShard(st, st.shard1.shardName);
                    });
                    const presentShardNames = [st.shard0.shardName, st.shard2.shardName];

                    // Flush the router cache to ensure insert succedes without retry.
                    assert.commandWorked(conn.adminCommand({flushRouterConfig: `${dbName}.${collName}`}));

                    // Insert after shard removal (data now on shard0).
                    const preDowngradeInsert = makeInsertCmd(dbName, collName);
                    preDowngradeInsert.execute(conn);

                    // Open IRS stream from before the mutations while FCV is still at latestFCV. The stream replays through chunk
                    // migrations and shard removal, entering bounded degraded segments.
                    const comment = `irs_degraded_downgrade_${scope}`;
                    cst = new ChangeStreamTest(db);
                    const cursor = openChangeStream(cst, {
                        watchMode,
                        version,
                        ignoreRemovedShards: true,
                        startAtOperationTime,
                        comment,
                    });
                    const v2CursorId = cursor.id;

                    // Consume the insert to advance the stream past the shard removal into a degraded segment. FCV must still be at
                    // latestFCV for this to work — the stream needs placement history to process the bounded segments.
                    cst.assertNextChangesEqual({
                        cursor,
                        expectedChanges: preDowngradeInsert.getChangeEvents(watchMode),
                    });

                    // After processing the shard removal, the stream targets only the remaining data shards.
                    assertOpenCursors(
                        st,
                        presentShardNames,
                        /* expectedConfigCursor */ isCluster,
                        cursorCommentFilter(comment),
                    );

                    // FCV downgrade.
                    new FCVDowngradeCommand().execute(conn);

                    // The v2 targeter detects placement is no longer available and transitions to Downgrading.
                    const switchCode = isCluster
                        ? V2TargeterLogCodes.kClusterShardTargeterPlacementNotAvailableSwitchToV1
                        : V2TargeterLogCodes.kShardTargeterDbPresentPlacementNotAvailableSwitchToV1;
                    awaitLogMessageCodes(
                        conn,
                        [switchCode, V2TargeterLogCodes.kTopologyHandlerStageStateTransition],
                        () => {
                            cst.assertNoChange(cursor);
                        },
                    );

                    // The next getMore detects Downgrading and throws RetryChangeStream, which reopens the cursor as v1.
                    const postDowngradeInsert = makeInsertCmd(dbName, collName);
                    postDowngradeInsert.execute(conn);
                    cst.assertNextChangesEqual({
                        cursor,
                        expectedChanges: postDowngradeInsert.getChangeEvents(watchMode),
                    });

                    // The stream was reopened as v1 after RetryChangeStream, so the cursor ID must differ.
                    const v1CursorId = cursor.id;
                    assert.neq(v1CursorId, v2CursorId, "cursor should have been reopened after FCV downgrade");

                    // v1 broadcasts to all present shards + config.
                    assertOpenCursors(
                        st,
                        presentShardNames,
                        /* expectedConfigCursor */ true,
                        cursorCommentFilter(comment),
                    );
                });
            }
        });
    });

    describe("FCV upgrade", function () {
        for (const watchMode of [
            ChangeStreamWatchMode.kCollection,
            ChangeStreamWatchMode.kDb,
            ChangeStreamWatchMode.kCluster,
        ]) {
            const scope = watchModeToString(watchMode);
            const isCluster = watchMode === ChangeStreamWatchMode.kCluster;

            it(`${scope}-scope: existing v1 stream stays v1 after FCV upgrade even when opened with version: v2`, function () {
                setupShardedCollection();

                // Downgrade FCV first.
                new FCVDowngradeCommand().execute(conn);

                // Use the config server's cluster time rather than the mongos's. After FCV downgrade, the mongos may
                // have a higher gossipped cluster time. If that time is ahead of the config server's configTime,
                // getAllocationToShardsStatus returns kFutureClusterTime instead of kNotAvailable, causing the stream
                // to open as v2 instead of v1.
                const startAtOperationTime = getClusterTime(st.configRS.getPrimary().getDB("admin"));

                const db = isCluster ? conn.getDB("admin") : conn.getDB(dbName);
                const comment = `upgrade_v1_stays_v1_${scope}`;
                cst = new ChangeStreamTest(db);

                // Open stream at downgraded FCV. It will be v1 even with version: "v2".
                const cursor = openChangeStream(cst, {watchMode, version, comment, startAtOperationTime});
                const preUpgradeCursorId = cursor.id;

                // Confirm the stream is alive and idle before checking cursor topology.
                cst.assertNoChange(cursor);

                // v1 broadcasts to all shards including the config server.
                assertOpenCursors(st, allShardNames, /* expectedConfigCursor */ true, cursorCommentFilter(comment));

                // FCV upgrade produces no user-visible events; the v1 stream is unaffected and delivers the insert normally.
                executeAndAssertEvents({
                    cst,
                    cursor,
                    conn,
                    watchMode,
                    cmds: [new FCVUpgradeCommand(), makeInsertCmd(dbName, collName)],
                });
                const postUpgradeCursorId = cursor.id;

                // v1 broadcasts to all shards including the config server.
                assertOpenCursors(st, allShardNames, /* expectedConfigCursor */ true, cursorCommentFilter(comment));

                // The v1 stream is unaffected by FCV upgrade, no RetryChangeStream, no cursor reopen. The cursor ID must remain the same.
                assert.eq(preUpgradeCursorId, postUpgradeCursorId, "cursor should remain the same after FCV upgrade");
            });

            it(`${scope}-scope: stream opened as v2 at downgraded FCV naturally falls back to v1 and stays v1 after upgrade`, function () {
                setupShardedCollection();

                // Downgrade FCV.
                new FCVDowngradeCommand().execute(conn);

                const db = isCluster ? conn.getDB("admin") : conn.getDB(dbName);
                const comment = `natural_v2_to_v1_upgrade_${scope}`;
                cst = new ChangeStreamTest(db);

                // Open without startAtOperationTime. The mongos may have a higher gossipped cluster time than the
                // config server, so getAllocationToShardsStatus returns kFutureClusterTime and the stream opens as v2.
                // On the first getMore the v2 state machine detects placement is unavailable and throws
                // RetryChangeStream, reopening the stream as v1.
                const cursor = openChangeStream(cst, {watchMode, version, comment});

                // Insert + consume to let the natural v2->v1 fallback happen.
                executeAndAssertEvents({
                    cst,
                    cursor,
                    conn,
                    watchMode,
                    cmds: [makeInsertCmd(dbName, collName)],
                });

                // After fallback, v1 broadcasts to all shards + config.
                assertOpenCursors(st, allShardNames, /* expectedConfigCursor */ true, cursorCommentFilter(comment));

                // FCV upgrade, the v1 stream should be unaffected.
                executeAndAssertEvents({
                    cst,
                    cursor,
                    conn,
                    watchMode,
                    cmds: [new FCVUpgradeCommand(), makeInsertCmd(dbName, collName)],
                });

                assertOpenCursors(st, allShardNames, /* expectedConfigCursor */ true, cursorCommentFilter(comment));
            });

            it(`${scope}-scope: v2 stream opened at pre-upgrade timestamp readjusts targeting after NamespacePlacementChanged`, function () {
                setupShardedCollection();

                // Start at downgraded FCV - placement history is not yet initialized.
                new FCVDowngradeCommand().execute(conn);

                const db = isCluster ? conn.getDB("admin") : conn.getDB(dbName);
                const startAtOperationTime = getClusterTime(db);

                // Insert a document.
                const preUpgradeInsert = makeInsertCmd(dbName, collName);
                preUpgradeInsert.execute(conn);
                const preUpgradeChanges = preUpgradeInsert.getChangeEvents(watchMode);

                // FCV upgrade.
                new FCVUpgradeCommand().execute(conn);

                // Open a v2 stream from the pre-upgrade operation time. FCV is now upgraded so placement history is
                // available. The v2 targeter initializes targeting all shards. Once it processes the NPC event from the
                // FCV upgrade, it learns that placement history is now being tracked and narrows the set of open cursors.
                const comment = `resume_before_fcv_upgrade_${scope}`;
                cst = new ChangeStreamTest(db);
                const cursor = openChangeStream(cst, {watchMode, version, startAtOperationTime, comment});
                const v2CursorId = cursor.id;

                const initCode = isCluster
                    ? V2TargeterLogCodes.kClusterShardTargeterInitStrictMode
                    : V2TargeterLogCodes.kCollOrDbShardTargeterInitStrictMode;
                awaitLogMessageCodes(conn, [initCode], () => {
                    cst.assertNextChangesEqual({cursor, expectedChanges: preUpgradeChanges});
                });

                // v2 targets all shards because placement history is not available at the
                // resume timestamp. Cluster scope also opens a config cursor.
                assertOpenCursors(
                    st,
                    allShardNames,
                    /* expectedConfigCursor */ isCluster,
                    cursorCommentFilter(comment),
                );

                // Insert a doc, then consume it. The getMore advances the stream past the NamespacePlacementChanged
                // entry written by FCV upgrade, causing the v2 targeter to call handlePlacementRefresh and readjust.
                const postUpgradeInsert = makeInsertCmd(dbName, collName);
                postUpgradeInsert.execute(conn);
                const postUpgradeChanges = postUpgradeInsert.getChangeEvents(watchMode);

                const placementRefreshCode = isCluster
                    ? V2TargeterLogCodes.kClusterPlacementRefresh
                    : V2TargeterLogCodes.kCollOrDbPlacementRefresh;
                awaitLogMessageCodes(conn, [placementRefreshCode], () => {
                    cst.assertNextChangesEqual({cursor, expectedChanges: postUpgradeChanges});
                });

                // After processing the placement event, v2 narrows to data-bearing shards only.
                assertOpenCursors(
                    st,
                    dataShardNames,
                    /* expectedConfigCursor */ isCluster,
                    cursorCommentFilter(comment),
                );

                // v2 stream stays v2, cursor id unchanged.
                assert.eq(cursor.id, v2CursorId, "cursor should remain the same after FCV upgrade");
            });

            it(`${scope}-scope: resume token from v1 stream remains valid after upgrade`, function () {
                // Start at downgraded FCV.
                new FCVDowngradeCommand().execute(conn);

                setupShardedCollection();

                const db = isCluster ? conn.getDB("admin") : conn.getDB(dbName);
                const comment = `resume_token_upgrade_${scope}`;

                cst = new ChangeStreamTest(db);

                // Open v1 stream and generate an event to capture a resume token.
                let resumeToken;
                {
                    const cursor = openChangeStream(cst, {watchMode, version, comment});

                    executeAndAssertEvents({cst, cursor, conn, watchMode, cmds: [makeInsertCmd(dbName, collName)]});

                    // At downgraded FCV, v1 broadcasts to all shards including the config server.
                    assertOpenCursors(st, allShardNames, /* expectedConfigCursor */ true, cursorCommentFilter(comment));

                    resumeToken = cst.getResumeToken(cursor);
                }

                // Upgrade FCV.
                new FCVUpgradeCommand().execute(conn);

                // Reopen with the v1 resume token. It should work as v2.
                {
                    const resumedComment = comment + "_resumed";
                    const cursor = openChangeStream(cst, {
                        watchMode,
                        version,
                        resumeAfter: resumeToken,
                        comment: resumedComment,
                    });

                    executeAndAssertEvents({cst, cursor, conn, watchMode, cmds: [makeInsertCmd(dbName, collName)]});

                    // Resumed stream runs as v2, i.e. targets only data-bearing shards. Cluster scope also opens a config cursor.
                    assertOpenCursors(
                        st,
                        dataShardNames,
                        /* expectedConfigCursor */ isCluster,
                        cursorCommentFilter(resumedComment),
                    );
                }
            });
        }
    });
});
