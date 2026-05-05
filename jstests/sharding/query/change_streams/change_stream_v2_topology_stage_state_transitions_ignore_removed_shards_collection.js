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

    describe("Collection-level change streams", () => {
        it("FetchingStartingChangeStreamSegment → FetchingDegradedGettingChangeEvent when opened at pre-removal timestamp", () => {
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

            // Capture T_before: a point in time when shard1 is still active and in the placement.
            // Wait for the cluster time to advance past it so it is not considered a future time.
            const T_before = waitForClusterTime(db, st);

            // Drain shard1 by moving its user-data chunk to shard2 and remove the shard. After removal,
            // shard1 is absent from the shard registry while the placement history at T_before still
            // records it.
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

            // Open the IRS change stream at T_before. The placement fetcher sees that shard1 appears in
            // the T_before placement but is no longer in the shard registry, so it returns a bounded
            // segment (nextPlacementChangedAt set). This causes FetchingStartingChangeStreamSegment to
            // transition to FetchingDegradedGettingChangeEvent instead of
            // FetchingNormalGettingChangeEvent. After the high-water mark advances past the segment-end
            // timestamp, the stage starts a new segment on the surviving shards and recovers into
            // FetchingNormalGettingChangeEvent.
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_initial_degraded";
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
                collection: coll,
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

        it("FetchingNormalGettingChangeEvent → FetchingDegradedGettingChangeEvent when processing a historical MoveChunk event for a removed shard", () => {
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

            // Capture T_before: both shard0 and shard1 are active and in the placement.
            // Neither has been removed, so startChangeStreamSegment(T_before) will return a
            // normal (unbounded) segment and the stage will enter FetchingNormalGettingChangeEvent.
            const T_before = waitForClusterTime(db, st);

            // Move shard0's chunk to shard2. This generates a MoveChunk oplog event at T_move1
            // (after T_before). Shard2 acts as a "doomed" intermediate shard that will be removed
            // before the change stream is opened.
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
            });

            // Open the IRS change stream at T_before. The placement at T_before is {shard0, shard1}
            // and both shards are still in the registry, so startChangeStreamSegment returns a
            // normal (unbounded) segment and the stage enters FetchingNormalGettingChangeEvent with
            // cursors on shard0 and shard1.
            //
            // The CS then processes the historical MoveChunk event at T_move1 (shard0 → shard2).
            // The stage tries to open a cursor on shard2, but shard2 has already been removed from
            // the shard registry.  This triggers ShardNotFound, which causes the transition
            // FetchingNormalGettingChangeEvent → FetchingDegradedGettingChangeEvent.
            //
            // From degraded mode the recovery path is:
            //   FetchingDegraded → FetchingStarting (at T_move1+1)
            //     startChangeStreamSegment(T_move1+1): placement {shard1, shard2}, shard2 gone
            //       → another bounded segment → FetchingDegraded
            //   FetchingDegraded → FetchingStarting (at T_move2)
            //     startChangeStreamSegment(T_move2): placement {shard1, shard3}, both present
            //       → normal segment → FetchingNormal
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_normal_to_degraded";
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
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

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

        it("FetchingNormalGettingChangeEvent → FetchingStartingChangeStreamSegment when all cursors disappear after processing a MoveChunk event for a removed shard", () => {
            // Set up a sharded collection with a single chunk on shard0 (shard0 is the primary and
            // receives the full range). Shard1 acts as the first "doomed" intermediate shard.
            assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            assert.commandWorked(coll.insert({_id: 0}));

            // Capture T_before: shard0 is the sole owner and is present in the shard registry.
            // startChangeStreamSegment(T_before) will return an unbounded segment for {shard0}, so
            // the stage enters FetchingNormalGettingChangeEvent with a single cursor on shard0.
            const T_before = waitForClusterTime(db, st);

            // Move shard0's chunk to shard1 at T_move1 (after T_before). Shard1 will be removed
            // before the change stream processes this event.
            assert.commandWorked(
                db.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {_id: 0},
                    to: st.shard1.shardName,
                    _waitForDelete: true,
                }),
            );

            // Drain shard1 by moving its chunk to shard2, then remove shard1.
            removeShardFromCluster(st, st.shard1.shardName, () => {
                assert.commandWorked(
                    db.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: {_id: 0},
                        to: st.shard2.shardName,
                        _waitForDelete: true,
                    }),
                );
            });

            // Open the IRS change stream at T_before.
            //
            // Phase 1 — Normal mode:
            //   startChangeStreamSegment(T_before): placement = {shard0}, shard0 present
            //   → unbounded segment → FetchingNormalGettingChangeEvent with cursor on shard0.
            //
            // Phase 2 — All-cursors-gone transition:
            //   CS processes historical MoveChunk(shard0 → shard1) at T_move1.
            //   Event handler: close cursor on shard0, request cursor on shard1.
            //   executeCursorRequests():
            //     (a) close shard0 → _currentlyTargetedDataShards = {}
            //     (b) open shard1 → shard1 is REMOVED → ShardNotFound
            //   After exception: _currentlyTargetedDataShards = {} (empty).
            //   → FetchingStartingChangeStreamSegment, NOT FetchingDegradedGettingChangeEvent,
            //     because there are no remaining open cursors.
            //
            // Phase 3 — Recovery via IRS segment fast-forward:
            //   startChangeStreamSegment(T_move1): placement = {shard1}, but shard1 is removed.
            //   All shards in the placement are gone, so the IRS placement fetcher does not return a
            //   bounded (degraded) segment.  Instead it advances past T_move1 to the next placement
            //   change (T_drain, when the chunk moved from shard1 to shard2), returning placement =
            //   {shard2} with openCursorAt = T_drain and no nextPlacementChangedAt.
            //   → unbounded segment → FetchingNormalGettingChangeEvent with cursor on shard2.
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_all_cursors_gone";
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
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            awaitV2StageStateTransitions(
                st.s,
                logOffset,
                [
                    {from: S.FetchingInitialization, to: S.FetchingStartingChangeStreamSegment},
                    {from: S.FetchingStartingChangeStreamSegment, to: S.FetchingNormalGettingChangeEvent},
                    {from: S.FetchingNormalGettingChangeEvent, to: S.FetchingStartingChangeStreamSegment},
                    {from: S.FetchingStartingChangeStreamSegment, to: S.FetchingNormalGettingChangeEvent},
                ],
                () => csTest.assertNoChange(csCursor),
            );

            assertOpenCursors(st, [st.shard2.shardName], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));
        });

        it("FetchingStartingChangeStreamSegment → FetchingNormalGettingChangeEvent directly when all shards in the initial segment are removed", () => {
            // Set up a sharded collection with a single chunk on shard0.
            assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            assert.commandWorked(coll.insert({_id: 0}));

            // Move shard0's chunk to shard1. Shard1 will be removed before the change stream
            // processes this event.
            assert.commandWorked(
                db.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {_id: 0},
                    to: st.shard1.shardName,
                    _waitForDelete: true,
                }),
            );

            // Capture T_before: shard1 is the sole owner. The placement history at T_before will
            // record only shard1.
            const T_before = waitForClusterTime(db, st);

            // Drain shard1 by moving its chunk to shard2, then remove shard1.
            // After removal, shard1 is absent from the shard registry. The collection's data now
            // lives on shard2.
            removeShardFromCluster(st, st.shard1.shardName, () => {
                assert.commandWorked(
                    db.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: {_id: 0},
                        to: st.shard2.shardName,
                        _waitForDelete: true,
                    }),
                );
            });

            // Open the IRS change stream at T_before.
            //
            // startChangeStreamSegment(T_before): placement at T_before = {shard1}, but shard1 is
            // no longer in the shard registry. All shards in the T_before placement are removed.
            //
            // The IRS placement fetcher detects that every shard is gone and does NOT return a
            // bounded (degraded) segment. Instead it loops internally, advances to the next
            // placement change (T_drain, when the chunk moved from shard1 to shard2), fetches the
            // placement there ({shard2}), confirms shard2 is present, and returns an unbounded
            // segment with openCursorAt = T_drain.
            //
            // The stage therefore transitions directly from FetchingStartingChangeStreamSegment to
            // FetchingNormalGettingChangeEvent, bypassing FetchingDegradedGettingChangeEvent
            // entirely.
            // This is the key distinction from the first test, where only SOME shards at T_before
            // were removed (leading to a bounded segment and a degraded cycle).
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_all_shards_removed_at_start";
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
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            awaitV2StageStateTransitions(
                st.s,
                logOffset,
                [
                    {from: S.FetchingInitialization, to: S.FetchingStartingChangeStreamSegment},
                    {from: S.FetchingStartingChangeStreamSegment, to: S.FetchingNormalGettingChangeEvent},
                ],
                () => csTest.assertNoChange(csCursor),
            );

            assertOpenCursors(st, [st.shard2.shardName], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));
        });

        it("FetchingStartingChangeStreamSegment → FetchingDegradedGettingChangeEvent when a shard in the collection placement is removed", () => {
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

            // Open a collection-level IRS change stream at T_before (collection: 1 means DB-level).
            // startChangeStreamSegment(T_before): placement = {shard0, shard1}, shard1 is removed
            // → some shards removed → bounded segment → FetchingDegradedGettingChangeEvent.
            // After the high-water mark advances past the segment-end timestamp, the stage starts a
            // new segment on the surviving shards ({shard0, shard2}) and recovers into
            // FetchingNormalGettingChangeEvent.
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_degraded";
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
                collection: coll,
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

        it("FetchingDegradedGettingChangeEvent undo: multiple documents spanning the segment boundary are all delivered", () => {
            // Set up a sharded collection: shard0 owns [MinKey, 0), shard1 owns [0, MaxKey].
            assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            distributeCollectionDataOverShards(db, coll, {
                middle: {_id: 0},
                chunks: [
                    {find: {_id: -1}, to: st.shard0.shardName},
                    {find: {_id: 1}, to: st.shard1.shardName},
                ],
            });

            // T_before: both shard0 and shard1 are active.
            const T_before = waitForClusterTime(db, st);

            // Insert two documents on the surviving shard (shard0) before the chunk move. Their
            // timestamps fall inside the degraded segment window [T_before, T_drain) and will be
            // delivered while the stage is in FetchingDegradedGettingChangeEvent. The subsequent
            // moveChunk creates the segment end timestamp T_drain.
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

            // Insert two more documents on shard0 after shard1's removal. Their timestamps exceed
            // T_drain (segmentEndTimestamp). When the stage reads one of these events in degraded
            // mode it detects overfetching, calls 'undoGetNext()' to push the event back into the
            // results merger, and transitions to FetchingStartingChangeStreamSegment. The event is
            // then re-delivered in the subsequent normal-mode segment.
            assert.commandWorked(coll.insert({_id: -30}));
            assert.commandWorked(coll.insert({_id: -40}));

            // Expected state-machine path:
            //   FetchingInitialization
            //   → FetchingStartingChangeStreamSegment  (T_before placement {shard0, shard1}; shard1
            //                                           gone → bounded segment; cursor on shard0 only)
            //   → FetchingDegradedGettingChangeEvent   (delivers {-10}, {-20}; then reads {-30} or
            //                                           a noop with timestamp >= T_drain → calls
            //                                           undoGetNext() or transitions on HWM crossing)
            //   → FetchingStartingChangeStreamSegment  (new segment from T_drain: {shard0, shard2}
            //                                           present → unbounded segment → normal mode)
            //   → FetchingNormalGettingChangeEvent     (delivers {-30} and {-40})
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_undo_docs_coll";
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
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            // Drive the state machine by reading all four expected insert events. Reads in degraded
            // mode, any undo triggered by overfetching, and the subsequent normal-mode recovery all
            // happen transparently; the consumer sees all four documents exactly once.
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

        it("Multiple documents on surviving shards delivered through Normal→Degraded→Normal recovery with two chunk moves", () => {
            // Set up a sharded collection: shard0 owns [MinKey, 0), shard1 owns [0, MaxKey].
            assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            distributeCollectionDataOverShards(db, coll, {
                middle: {_id: 0},
                chunks: [
                    {find: {_id: -1}, to: st.shard0.shardName},
                    {find: {_id: 1}, to: st.shard1.shardName},
                ],
            });

            // T_before: both shard0 and shard1 present → initial normal segment opens on
            // {shard0, shard1}.
            const T_before = waitForClusterTime(db, st);

            // Insert two documents on shard1 (which is never removed and always in the placement)
            // before any chunk moves. Their timestamps are in the initial normal window and will be
            // returned while the stage is in FetchingNormalGettingChangeEvent.
            assert.commandWorked(coll.insert({_id: 100}));
            assert.commandWorked(coll.insert({_id: 200}));

            // Move shard0's chunk through the doomed intermediate shard2 on to shard3, then remove
            // shard2. By the time the change stream processes the MoveChunk(shard0→shard2) control
            // event, shard2 is already absent from the shard registry. The resulting ShardNotFound
            // triggers Normal→Degraded. Recovery requires two FetchingStartingChangeStreamSegment
            // cycles to advance past both placement changes (T_move1 and T_move2).
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
            });

            // Insert documents on the two surviving shards after full recovery. These are delivered
            // once the stage reaches the final FetchingNormalGettingChangeEvent segment.
            assert.commandWorked(coll.insert({_id: 300})); // shard1 keeps [0, MaxKey)
            assert.commandWorked(coll.insert({_id: -300})); // shard3 now owns [MinKey, 0)

            // Open the IRS change stream at T_before.
            //
            // Recovery path:
            //   FetchingNormal:  delivers {100}, {200} from shard1;
            //                    processes MoveChunk(shard0→shard2) → ShardNotFound → Degraded
            //   FetchingDegraded → FetchingStarting(T_move1):
            //                    placement {shard1, shard2}, shard2 gone → bounded segment → Degraded
            //   FetchingDegraded → FetchingStarting(T_move2):
            //                    placement {shard1, shard3}, both present → Normal
            //   FetchingNormal:  delivers {300} from shard1 and {-300} from shard3
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_multi_docs_normal_to_degraded_coll";
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
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            // Read all four expected inserts; the Normal→Degraded cycle and subsequent recovery
            // happen transparently and all four documents are delivered exactly once.
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
