/**
 * White-box integration tests for IRS state transitions in ChangeStreamHandleTopologyChangeV2Stage
 * that require shard removal. Observes state transitions externally via LOGV2 log ID 10657506,
 * which is emitted on every state transition with `attr.previous` and `attr.new` containing the
 * string state names.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   config_shard_incompatible,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_fcv_90,
 *   requires_sharding,
 *   uses_change_streams,
 *   # Exclude the test from running in very slow build variants.
 *   tsan_incompatible,
 *   incompatible_aubsan,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it, before, beforeEach, after, afterEach} from "jstests/libs/mochalite.js";
import {
    ChangeStreamTest,
    assertOpenCursors,
    cursorCommentFilter,
    distributeCollectionDataOverShards,
    removeShardFromCluster,
    awaitV2StageStateTransitions,
    waitForClusterTime,
} from "jstests/libs/query/change_stream_util.js";

// String names produced by stateToString() and recorded in log attr.previous / attr.new.
const S = Object.freeze({
    Uninitialized: "Uninitialized",
    Waiting: "Waiting",
    FetchingInitialization: "FetchingInitialization",
    FetchingGettingChangeEvent: "FetchingGettingChangeEvent",
    FetchingStartingChangeStreamSegment: "FetchingStartingChangeStreamSegment",
    FetchingNormalGettingChangeEvent: "FetchingNormalGettingChangeEvent",
    FetchingDegradedGettingChangeEvent: "FetchingDegradedGettingChangeEvent",
});

describe("ChangeStreamHandleTopologyChangeV2Stage: IRS degraded-mode state transitions", () => {
    // jsTestName() can exceed the 64-character limit for database names.
    const dbName = "transitions_test";

    let st;
    let db;
    let coll;
    let csTest;

    let savedSkipIndexCheck;

    before(() => {
        savedSkipIndexCheck = TestData.skipCheckingIndexesConsistentAcrossCluster;
    });

    after(() => {
        TestData.skipCheckingIndexesConsistentAcrossCluster = savedSkipIndexCheck;
    });

    beforeEach(() => {
        // Temporarily disable index-consistency checks because they fail when a shard has been
        // removed from the cluster.
        TestData.skipCheckingIndexesConsistentAcrossCluster = true;

        // shard3 is a survivor shard: it is never removed and ensures that the cluster has at least
        // one shard remaining for a clean shutdown.
        st = new ShardingTest({
            shards: 4,
            mongos: 1,
            config: 1,
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
            other: {
                enableBalancer: false,
                configOptions: {
                    setParameter: {
                        writePeriodicNoops: true,
                        periodicNoopIntervalSecs: 1,
                    },
                },
            },
        });

        db = st.s.getDB(dbName);
        db.dropDatabase();
        coll = db.test;
    });

    afterEach(() => {
        if (csTest) {
            csTest.cleanUp();
            csTest = null;
        }
        db.dropDatabase();
        st.stop();
    });

    describe("Database-level change streams", () => {
        it("FetchingStartingChangeStreamSegment → FetchingDegradedGettingChangeEvent when a shard in the database placement is removed", () => {
            // Set up a sharded collection with chunks on shard0 and shard1.
            assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            distributeCollectionDataOverShards(db, coll, {
                middle: {_id: 0},
                chunks: [
                    {find: {_id: -1}, to: st.shard0.shardName},
                    {find: {_id: 1}, to: st.shard1.shardName},
                ],
            });

            // Capture T_before: both shard0 and shard1 are active and in the database placement.
            const T_before = waitForClusterTime(db, st);

            // Drain shard1 by moving its user-data chunk ({_id: 1}) to shard2, then remove shard1.
            // After removal, the placement history at T_before still records {shard0, shard1}.
            removeShardFromCluster(st, st.shard1.shardName, () => {
                assert.commandWorked(
                    db.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: {_id: 1},
                        to: st.shard2.shardName,
                        _waitForDelete: true,
                    }),
                );
            });

            // Open a DB-level IRS change stream at T_before (collection: 1 means DB-level).
            // startChangeStreamSegment(T_before): placement = {shard0, shard1}, shard1 is removed
            // → some shards removed → bounded segment → FetchingDegradedGettingChangeEvent.
            // After the high-water mark advances past the segment-end timestamp, the stage starts a
            // new segment on the surviving shards ({shard0, shard2}) and recovers into
            // FetchingNormalGettingChangeEvent.
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_db_irs_degraded";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [
                    {
                        $changeStream: {
                            version: "v2",
                            startAtOperationTime: T_before,
                            ignoreRemovedShards: true,
                        },
                    },
                ],
                collection: 1,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            awaitV2StageStateTransitions(
                st.s,
                logOffset,
                [
                    {from: S.FetchingInitialization, to: S.FetchingStartingChangeStreamSegment},
                    {from: S.FetchingStartingChangeStreamSegment, to: S.FetchingDegradedGettingChangeEvent},
                    {from: S.FetchingDegradedGettingChangeEvent, to: S.FetchingStartingChangeStreamSegment},
                    {from: S.FetchingStartingChangeStreamSegment, to: S.FetchingNormalGettingChangeEvent},
                ],
                () => csTest.assertNoChange(csCursor),
            );

            assertOpenCursors(
                st,
                [st.shard0.shardName, st.shard2.shardName],
                /*expectedConfigCursor=*/ false,
                cursorCommentFilter(comment),
            );
        });

        it("FetchingDegradedGettingChangeEvent undo: multiple documents spanning the segment boundary are all delivered at database level", () => {
            // Same scenario as the collection-level undo test but using a DB-level change stream.
            assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            distributeCollectionDataOverShards(db, coll, {
                middle: {_id: 0},
                chunks: [
                    {find: {_id: -1}, to: st.shard0.shardName},
                    {find: {_id: 1}, to: st.shard1.shardName},
                ],
            });

            const T_before = waitForClusterTime(db, st);

            // Insert two documents inside the degraded window (before the chunk move) and two after
            // T_drain. The two post-T_drain documents exercise the undoGetNext() path: the stage
            // overfetches them in degraded mode, pushes them back, and re-delivers them in the
            // subsequent normal-mode segment.
            removeShardFromCluster(st, st.shard1.shardName, () => {
                assert.commandWorked(coll.insert({_id: -10}));
                assert.commandWorked(coll.insert({_id: -20}));
                assert.commandWorked(
                    db.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: {_id: 1},
                        to: st.shard2.shardName,
                        _waitForDelete: true,
                    }),
                );
            });
            assert.commandWorked(coll.insert({_id: -30}));
            assert.commandWorked(coll.insert({_id: -40}));

            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_undo_docs_db";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [
                    {
                        $changeStream: {
                            version: "v2",
                            startAtOperationTime: T_before,
                            ignoreRemovedShards: true,
                        },
                    },
                ],
                collection: 1,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            const changes = csTest.getNextChanges(csCursor, 4, true);
            assert.sameMembers(
                changes.map((c) => c.fullDocument._id),
                [-10, -20, -30, -40],
            );

            awaitV2StageStateTransitions(
                st.s,
                logOffset,
                [
                    {from: S.FetchingInitialization, to: S.FetchingStartingChangeStreamSegment},
                    {from: S.FetchingStartingChangeStreamSegment, to: S.FetchingDegradedGettingChangeEvent},
                    {from: S.FetchingDegradedGettingChangeEvent, to: S.FetchingStartingChangeStreamSegment},
                    {from: S.FetchingStartingChangeStreamSegment, to: S.FetchingNormalGettingChangeEvent},
                ],
                () => csTest.assertNoChange(csCursor),
            );

            assertOpenCursors(
                st,
                [st.shard0.shardName, st.shard2.shardName],
                /*expectedConfigCursor=*/ false,
                cursorCommentFilter(comment),
            );
        });

        it("Multiple documents on surviving shards delivered through Normal→Degraded→Normal recovery with two chunk moves at database level", () => {
            // Same scenario as the collection-level Normal→Degraded multi-doc test but using a
            // DB-level change stream.
            assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            distributeCollectionDataOverShards(db, coll, {
                middle: {_id: 0},
                chunks: [
                    {find: {_id: -1}, to: st.shard0.shardName},
                    {find: {_id: 1}, to: st.shard1.shardName},
                ],
            });

            const T_before = waitForClusterTime(db, st);

            // Documents on shard1 (never removed) inserted before the chunk moves will be delivered
            // in the initial normal segment.
            assert.commandWorked(coll.insert({_id: 100}));
            assert.commandWorked(coll.insert({_id: 200}));

            removeShardFromCluster(st, st.shard2.shardName, () => {
                assert.commandWorked(
                    db.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: {_id: -1},
                        to: st.shard2.shardName,
                        _waitForDelete: true,
                    }),
                );
                assert.commandWorked(
                    db.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: {_id: -1},
                        to: st.shard3.shardName,
                        _waitForDelete: true,
                    }),
                );

                // Also move the primary for the database to shard3, so there are no leftovers on
                // shard0.
                assert.commandWorked(
                    db.adminCommand({
                        movePrimary: dbName,
                        to: st.shard3.shardName,
                    }),
                );
            });

            // After full recovery the stage enters normal mode on {shard1, shard3}; documents
            // inserted on those two shards are delivered in the final segment.
            assert.commandWorked(coll.insert({_id: 300}));
            assert.commandWorked(coll.insert({_id: -300}));

            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_multi_docs_normal_to_degraded_db";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [
                    {
                        $changeStream: {
                            version: "v2",
                            startAtOperationTime: T_before,
                            ignoreRemovedShards: true,
                        },
                    },
                ],
                collection: 1,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            const changes = csTest.getNextChanges(csCursor, 4, true);
            assert.sameMembers(
                changes.map((c) => c.fullDocument._id),
                [100, 200, 300, -300],
            );

            awaitV2StageStateTransitions(
                st.s,
                logOffset,
                [
                    {from: S.FetchingInitialization, to: S.FetchingStartingChangeStreamSegment},
                    {from: S.FetchingStartingChangeStreamSegment, to: S.FetchingNormalGettingChangeEvent},
                    {from: S.FetchingNormalGettingChangeEvent, to: S.FetchingDegradedGettingChangeEvent},
                    {from: S.FetchingDegradedGettingChangeEvent, to: S.FetchingStartingChangeStreamSegment},
                    {from: S.FetchingStartingChangeStreamSegment, to: S.FetchingDegradedGettingChangeEvent},
                    {from: S.FetchingDegradedGettingChangeEvent, to: S.FetchingStartingChangeStreamSegment},
                    {from: S.FetchingStartingChangeStreamSegment, to: S.FetchingNormalGettingChangeEvent},
                ],
                () => csTest.assertNoChange(csCursor),
            );

            assertOpenCursors(
                st,
                [st.shard1.shardName, st.shard3.shardName],
                /*expectedConfigCursor=*/ false,
                cursorCommentFilter(comment),
            );
        });
    });
});
