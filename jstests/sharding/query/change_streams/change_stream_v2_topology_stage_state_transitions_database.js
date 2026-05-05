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
    waitForClusterTime,
} from "jstests/libs/query/change_stream_util.js";
import {
    CreateDatabaseCommand,
    CreateUntrackedCollectionCommand,
    ShardCollectionCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";

const {kCollOrDbPlacementRefresh} = V2TargeterLogCodes;

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

    describe("Database-level change streams", () => {
        it("strict mode init: Uninitialized → FetchingInitialization → FetchingGettingChangeEvent", () => {
            jsTest.log.info(
                "Database-level change streams: strict mode init: Uninitialized → FetchingInitialization → FetchingGettingChangeEvent",
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

            // Clear logs so they don't get too large.
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));

            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_db_strict_init";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [{$changeStream: {version: "v2", startAtOperationTime: currentTime}}],
                collection: 1,
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
                "Database-level change streams: IRS mode init: Uninitialized → FetchingInitialization → FetchingStartingChangeStreamSegment → FetchingNormalGettingChangeEvent",
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

            // Clear logs so they don't get too large.
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));

            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_db_irs_init";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [
                    {
                        $changeStream: {
                            version: "v2",
                            startAtOperationTime: currentTime,
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
                    {from: S.Uninitialized, to: S.FetchingInitialization},
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

        it("strict mode: stays in FetchingGettingChangeEvent during movePrimary", () => {
            jsTest.log.info(
                "Database-level change streams: strict mode: stays in FetchingGettingChangeEvent during movePrimary",
            );

            new CreateUntrackedCollectionCommand({dbName, collName}).execute(st.s);
            coll = db[collName];

            // Wait for cluster time on the config server to advance to a point so that a change stream
            // opened with startAtOperationTime = currentTime is not considered a future cluster time.
            const currentTime = waitForClusterTime(db, st);
            const comment = "state_trans_db_strict_moveprimary";
            csTest = new ChangeStreamTest(db);

            // Clear logs so they don't get too large.
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));

            const logOffset = checkLog.getGlobalLog(st.s).length;
            const csCursor = csTest.startWatchingChanges({
                pipeline: [{$changeStream: {version: "v2", startAtOperationTime: currentTime}}],
                collection: 1,
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

            const logOffsetAfterInit = checkLog.getGlobalLog(st.s).length;

            assert.commandWorked(st.s.adminCommand({movePrimary: db.getName(), to: st.shard1.shardName}));
            awaitLogMessageCodes(st.s, [kCollOrDbPlacementRefresh], () => csTest.assertNoChange(csCursor));

            // The stage must not have left FetchingGettingChangeEvent.
            assertNoV2StageStateTransitionFrom(st.s, logOffsetAfterInit, S.FetchingGettingChangeEvent);
            assertOpenCursors(st, [st.shard1.shardName], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));
        });

        it("IRS mode: stays in FetchingNormalGettingChangeEvent during movePrimary", () => {
            jsTest.log.info(
                "Database-level change streams: IRS mode: stays in FetchingNormalGettingChangeEvent during movePrimary",
            );

            new CreateUntrackedCollectionCommand({dbName, collName}).execute(st.s);
            coll = db[collName];

            // Wait for cluster time on the config server to advance to a point so that a change stream
            // opened with startAtOperationTime = currentTime is not considered a future cluster time.
            const currentTime = waitForClusterTime(db, st);

            // Clear logs so they don't get too large.
            assert.commandWorked(st.s.adminCommand({clearLog: "global"}));

            const logOffset = checkLog.getGlobalLog(st.s).length;
            const comment = "state_trans_db_irs_moveprimary";
            csTest = new ChangeStreamTest(db);
            const csCursor = csTest.startWatchingChanges({
                pipeline: [
                    {$changeStream: {version: "v2", startAtOperationTime: currentTime, ignoreRemovedShards: true}},
                ],
                collection: 1,
                aggregateOptions: {comment, cursor: {batchSize: 0}},
            });

            awaitV2StageStateTransitions(
                st.s,
                logOffset,
                [{from: S.FetchingStartingChangeStreamSegment, to: S.FetchingNormalGettingChangeEvent}],
                () => csTest.assertNoChange(csCursor),
            );

            const logOffsetAfterInit = checkLog.getGlobalLog(st.s).length;

            assert.commandWorked(st.s.adminCommand({movePrimary: db.getName(), to: st.shard1.shardName}));
            awaitLogMessageCodes(st.s, [kCollOrDbPlacementRefresh], () => csTest.assertNoChange(csCursor));

            // The stage must not have left FetchingNormalGettingChangeEvent.
            assertNoV2StageStateTransitionFrom(st.s, logOffsetAfterInit, S.FetchingNormalGettingChangeEvent);
            assertOpenCursors(st, [st.shard1.shardName], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));
        });
    });
});
