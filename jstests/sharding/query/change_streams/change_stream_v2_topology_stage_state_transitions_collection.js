/**
 * White-box integration tests for state transitions in ChangeStreamHandleTopologyChangeV2Stage.
 * Observes state transitions externally via LOGV2 log ID 10657506, which is emitted on every state
 * transition with `attr.previous` and `attr.new` containing the string state names. Open/closed
 * cursor assertions are used as additional cross-checks.
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
    distributeCollectionDataOverShards,
    assertOpenCursors,
    cursorCommentFilter,
    assertNoV2StageStateTransitionFrom,
    awaitLogMessageCodes,
    awaitV2StageStateTransitions,
    V2TargeterLogCodes,
    getClusterTime,
    waitForClusterTime,
} from "jstests/libs/query/change_stream_util.js";
import {
    CreateDatabaseCommand,
    CreateUntrackedCollectionCommand,
    ShardCollectionCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";

const {kCollectionHandleMoveChunk: kHandleMoveChunk} = V2TargeterLogCodes;

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

describe("ChangeStreamHandleTopologyChangeV2Stage: state transitions", () => {
    const dbName = "transitions_test";
    const collName = "test";

    let st;
    let db;
    let coll;
    let csTest;
    let allShards;

    before(() => {
        st = new ShardingTest({
            shards: 3,
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
        allShards = assert.commandWorked(st.s.adminCommand({listShards: 1})).shards;
    });

    after(() => {
        st.stop();
    });

    beforeEach(() => {
        // Clear logs so they don't get too large.
        assert.commandWorked(st.s.adminCommand({clearLog: "global"}));

        // Recreate database on shard0.
        new CreateDatabaseCommand({dbName, primaryShard: st.shard0.shardName}).execute(st.s);
    });

    afterEach(() => {
        if (csTest) {
            csTest.cleanUp();
            csTest = null;
        }
        db.dropDatabase();
    });

    describe("Collection-level change streams", () => {
        it("strict mode init: Uninitialized → FetchingInitialization → FetchingGettingChangeEvent", () => {
            jsTest.log.info(
                "Collection-level change streams: strict mode init: Uninitialized → FetchingInitialization → FetchingGettingChangeEvent",
            );

            new ShardCollectionCommand({
                dbName,
                collName,
                shardSet: allShards,
                collectionCtx: {exists: false},
                shardKey: {_id: 1},
            }).execute(st.s);
            coll = db[collName];
            distributeCollectionDataOverShards(db, coll, {
                middle: {_id: 0},
                chunks: [
                    {find: {_id: -1}, to: st.shard0.shardName},
                    {find: {_id: 1}, to: st.shard1.shardName},
                ],
            });

            // Wait for cluster time on the config server to advance to a point so that a change stream
            // opened with startAtOperationTime = currentTime is not considered a future cluster time.
            const currentTime = waitForClusterTime(db, st);
            const comment = "state_trans_strict_init";
            const logOffset = checkLog.getGlobalLog(st.s).length;
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [{$changeStream: {version: "v2", startAtOperationTime: currentTime}}],
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            awaitV2StageStateTransitions(
                st.s,
                logOffset,
                [
                    {from: S.Uninitialized, to: S.FetchingInitialization},
                    {from: S.FetchingInitialization, to: S.FetchingGettingChangeEvent},
                ],
                () => csTest.assertNoChange(csCursor),
            );

            assertOpenCursors(
                st,
                [st.shard0.shardName, st.shard1.shardName],
                /*expectedConfigCursor=*/ false,
                cursorCommentFilter(comment),
            );
        });

        it("IRS mode init: Uninitialized → FetchingInitialization → FetchingStartingChangeStreamSegment → FetchingNormalGettingChangeEvent", () => {
            jsTest.log.info(
                "Collection-level change streams: IRS mode init: Uninitialized → FetchingInitialization → FetchingStartingChangeStreamSegment → FetchingNormalGettingChangeEvent",
            );

            new ShardCollectionCommand({
                dbName,
                collName,
                shardSet: allShards,
                collectionCtx: {exists: false},
                shardKey: {_id: 1},
            }).execute(st.s);
            coll = db[collName];
            distributeCollectionDataOverShards(db, coll, {
                middle: {_id: 0},
                chunks: [
                    {find: {_id: -1}, to: st.shard0.shardName},
                    {find: {_id: 1}, to: st.shard1.shardName},
                ],
            });

            // Wait for cluster time on the config server to advance to a point so that a change stream
            // opened with startAtOperationTime = currentTime is not considered a future cluster time.
            const currentTime = waitForClusterTime(db, st);
            const comment = "state_trans_irs_init";
            const logOffset = checkLog.getGlobalLog(st.s).length;
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [
                    {$changeStream: {version: "v2", startAtOperationTime: currentTime, ignoreRemovedShards: true}},
                ],
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            awaitV2StageStateTransitions(
                st.s,
                logOffset,
                [
                    {from: S.Uninitialized, to: S.FetchingInitialization},
                    {
                        from: S.FetchingInitialization,
                        to: S.FetchingStartingChangeStreamSegment,
                    },
                    {
                        from: S.FetchingStartingChangeStreamSegment,
                        to: S.FetchingNormalGettingChangeEvent,
                    },
                ],
                () => csTest.assertNoChange(csCursor),
            );

            assertOpenCursors(
                st,
                [st.shard0.shardName, st.shard1.shardName],
                /*expectedConfigCursor=*/ false,
                cursorCommentFilter(comment),
            );
        });

        it("strict mode: Uninitialized → Waiting when opening at a future cluster time", () => {
            jsTest.log.info(
                "Collection-level change streams: strict mode: Uninitialized → Waiting when opening at a future cluster time",
            );

            new CreateUntrackedCollectionCommand({dbName, collName}).execute(st.s);
            coll = db[collName];

            // Open at a timestamp 60 s in the future so the config server's placement data is not yet
            // available, causing the stage to enter the Waiting state.
            const currentTime = getClusterTime(db);
            const futureTime = new Timestamp(currentTime.getTime() + 60, 0);
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_strict_waiting";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [{$changeStream: {version: "v2", startAtOperationTime: futureTime}}],
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            awaitV2StageStateTransitions(st.s, logOffset, [{from: S.Uninitialized, to: S.Waiting}], () =>
                csTest.assertNoChange(csCursor),
            );
        });

        it("IRS mode: Uninitialized → Waiting when opening at a future cluster time", () => {
            jsTest.log.info(
                "Collection-level change streams: IRS mode: Uninitialized → Waiting when opening at a future cluster time",
            );

            new CreateUntrackedCollectionCommand({dbName, collName}).execute(st.s);
            coll = db[collName];

            // Open at a timestamp 60 s in the future so the config server's placement data is not yet
            // available, causing the stage to enter the Waiting state.
            const currentTime = getClusterTime(db);
            const futureTime = new Timestamp(currentTime.getTime() + 60, 0);
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_waiting";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [
                    {$changeStream: {version: "v2", startAtOperationTime: futureTime, ignoreRemovedShards: true}},
                ],
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            awaitV2StageStateTransitions(st.s, logOffset, [{from: S.Uninitialized, to: S.Waiting}], () =>
                csTest.assertNoChange(csCursor),
            );
        });

        it("strict mode: Waiting → FetchingInitialization → FetchingGettingChangeEvent when future time arrives", () => {
            jsTest.log.info(
                "Collection-level change streams: strict mode: Waiting → FetchingInitialization → FetchingGettingChangeEvent when future time arrives",
            );

            new ShardCollectionCommand({
                dbName,
                collName,
                shardSet: allShards,
                collectionCtx: {exists: false},
                shardKey: {_id: 1},
            }).execute(st.s);
            coll = db[collName];
            distributeCollectionDataOverShards(db, coll, {
                middle: {_id: 0},
                chunks: [
                    {find: {_id: -1}, to: st.shard0.shardName},
                    {find: {_id: 1}, to: st.shard1.shardName},
                ],
            });

            // Use a near-future offset so the stage enters Waiting initially, then
            // transitions to FetchingInitialization once the cluster time advances past it.
            const currentTime = getClusterTime(db);
            const nearFutureTime = new Timestamp(currentTime.getTime() + 10, 0);
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_strict_waiting_cycle";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [{$changeStream: {version: "v2", startAtOperationTime: nearFutureTime}}],
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            awaitV2StageStateTransitions(
                st.s,
                logOffset,
                [
                    {from: S.Uninitialized, to: S.Waiting},
                    {from: S.Waiting, to: S.FetchingInitialization},
                    {from: S.FetchingInitialization, to: S.FetchingGettingChangeEvent},
                ],
                () => csTest.assertNoChange(csCursor),
            );

            assertOpenCursors(
                st,
                [st.shard0.shardName, st.shard1.shardName],
                /*expectedConfigCursor=*/ false,
                cursorCommentFilter(comment),
            );
        });

        it("IRS mode: Waiting → FetchingInitialization → FetchingStartingChangeStreamSegment → FetchingNormalGettingChangeEvent when future time arrives", () => {
            jsTest.log.info(
                "Collection-level change streams: IRS mode: Waiting → FetchingInitialization → FetchingStartingChangeStreamSegment → FetchingNormalGettingChangeEvent when future time arrives",
            );

            new ShardCollectionCommand({
                dbName,
                collName,
                shardSet: allShards,
                collectionCtx: {exists: false},
                shardKey: {_id: 1},
            }).execute(st.s);
            coll = db[collName];
            distributeCollectionDataOverShards(db, coll, {
                middle: {_id: 0},
                chunks: [
                    {find: {_id: -1}, to: st.shard0.shardName},
                    {find: {_id: 1}, to: st.shard1.shardName},
                ],
            });

            const currentTime = getClusterTime(db);
            const nearFutureTime = new Timestamp(currentTime.getTime() + 10, 0);
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_waiting_cycle";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [
                    {
                        $changeStream: {
                            version: "v2",
                            startAtOperationTime: nearFutureTime,
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
                    {from: S.Uninitialized, to: S.Waiting},
                    {from: S.Waiting, to: S.FetchingInitialization},
                    {from: S.FetchingInitialization, to: S.FetchingStartingChangeStreamSegment},
                    {from: S.FetchingStartingChangeStreamSegment, to: S.FetchingNormalGettingChangeEvent},
                ],
                () => csTest.assertNoChange(csCursor),
            );

            assertOpenCursors(
                st,
                [st.shard0.shardName, st.shard1.shardName],
                /*expectedConfigCursor=*/ false,
                cursorCommentFilter(comment),
            );
        });

        it("strict mode: stays in FetchingGettingChangeEvent during moveChunk", () => {
            jsTest.log.info(
                "Collection-level change streams: strict mode: stays in FetchingGettingChangeEvent during moveChunk",
            );

            coll = db[collName];
            assert.commandWorked(db.createCollection(coll.getName()));
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
            assert.commandWorked(
                db.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {_id: -1},
                    to: st.shard0.shardName,
                    _waitForDelete: true,
                }),
            );
            assert.commandWorked(
                db.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {_id: 1},
                    to: st.shard1.shardName,
                    _waitForDelete: true,
                }),
            );

            const currentTime = getClusterTime(db);
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_strict_movechunk";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [{$changeStream: {version: "v2", startAtOperationTime: currentTime}}],
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            awaitV2StageStateTransitions(
                st.s,
                logOffset,
                [{from: S.FetchingInitialization, to: S.FetchingGettingChangeEvent}],
                () => csTest.assertNoChange(csCursor),
            );

            const logOffsetAfterInit = checkLog.getGlobalLog(st.s).length;

            // Move a chunk to shard2 and wait for the stage to process the resulting
            // NamespacePlacementChanged event.
            assert.commandWorked(
                db.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {_id: 1},
                    to: st.shard2.shardName,
                    _waitForDelete: true,
                }),
            );
            awaitLogMessageCodes(st.s, [kHandleMoveChunk], () => csTest.assertNoChange(csCursor));

            // The stage must not have left FetchingGettingChangeEvent.
            assertNoV2StageStateTransitionFrom(st.s, logOffsetAfterInit, S.FetchingGettingChangeEvent);
        });

        it("IRS mode: stays in FetchingNormalGettingChangeEvent during moveChunk", () => {
            jsTest.log.info(
                "Collection-level change streams: IRS mode: stays in FetchingNormalGettingChangeEvent during moveChunk",
            );

            coll = db[collName];
            assert.commandWorked(db.createCollection(coll.getName()));
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
            assert.commandWorked(
                db.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {_id: -1},
                    to: st.shard0.shardName,
                    _waitForDelete: true,
                }),
            );
            assert.commandWorked(
                db.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {_id: 1},
                    to: st.shard1.shardName,
                    _waitForDelete: true,
                }),
            );

            const currentTime = getClusterTime(db);
            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_irs_movechunk";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [
                    {$changeStream: {version: "v2", startAtOperationTime: currentTime, ignoreRemovedShards: true}},
                ],
                collection: coll,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            awaitV2StageStateTransitions(
                st.s,
                logOffset,
                [
                    {
                        from: S.FetchingStartingChangeStreamSegment,
                        to: S.FetchingNormalGettingChangeEvent,
                    },
                ],
                () => csTest.assertNoChange(csCursor),
            );

            const logOffsetAfterInit = checkLog.getGlobalLog(st.s).length;

            // Move a chunk to shard2 and wait for the stage to process the resulting
            // NamespacePlacementChanged event.
            assert.commandWorked(
                db.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {_id: 1},
                    to: st.shard2.shardName,
                    _waitForDelete: true,
                }),
            );
            awaitLogMessageCodes(st.s, [kHandleMoveChunk], () => csTest.assertNoChange(csCursor));

            // The stage must not have left FetchingNormalGettingChangeEvent.
            assertNoV2StageStateTransitionFrom(st.s, logOffsetAfterInit, S.FetchingNormalGettingChangeEvent);
        });
    });
});
