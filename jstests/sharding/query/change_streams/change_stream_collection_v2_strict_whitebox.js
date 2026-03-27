/**
 * White-box integration tests for collection-level change stream v2 shard targeting in strict mode.
 * Verifies observable shard-targeting behavior: which shards have open cursors after each lifecycle
 * event, and that placement history is consulted at the right times.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it, before, beforeEach, after, afterEach} from "jstests/libs/mochalite.js";
import {
    ChangeStreamTest,
    distributeCollectionDataOverShards,
    assertOpenCursors,
    cursorCommentFilter,
    awaitLogMessageCodes,
} from "jstests/libs/query/change_stream_util.js";
import {
    CreateDatabaseCommand,
    CreateUntrackedCollectionCommand,
    InsertDocCommand,
    ShardCollectionCommand,
    ReshardCollectionCommand,
    MovePrimaryCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";

// LOGV2 IDs used in assertions.
const kInitStrictMode = 11600500; // Strict mode initialization
const kPlacementRefresh = 10922912; // handlePlacementRefresh (MovePrimary, NamespacePlacementChanged)
const kDbAbsentEvent = 12013809; // DbAbsent event handling (DatabaseCreated)
const kHandleMoveChunk = 10917004; // Collection-level handleMoveChunk

describe("collection v2 strict whitebox", function () {
    let st;
    let db;
    let coll;
    let csTest;
    let allShards;

    const dbName = jsTestName();
    const collName = "test";

    before(function () {
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

    after(function () {
        st.stop();
    });

    beforeEach(function () {
        new CreateDatabaseCommand(dbName, null, null, null, st.shard0.shardName).execute(st.s);
    });

    afterEach(function () {
        if (csTest) {
            csTest.cleanUp();
            csTest = null;
        }
        db.dropDatabase();
    });

    function assertExpectedShardsInLog(attr, expectedShardsCount) {
        assert.eq(
            attr.shards.length,
            expectedShardsCount,
            `Expected ${expectedShardsCount} shards, got ${tojsononeline(attr.shards)}`,
        );
    }

    it("sharded collection across two shards - initial placement", function () {
        coll = db[collName];
        new ShardCollectionCommand(dbName, collName, allShards, {exists: false}, {_id: 1}).execute(st.s);
        distributeCollectionDataOverShards(db, coll, {
            middle: {_id: 0},
            chunks: [
                {find: {_id: -1}, to: st.shard0.shardName},
                {find: {_id: 1}, to: st.shard1.shardName},
            ],
        });

        const comment = "strict_initial_placement";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: coll,
            aggregateOptions: {comment, cursor: {batchSize: 0}},
        });

        awaitLogMessageCodes(st.s, [kInitStrictMode], () => csTest.assertNoChange(csCursor), {
            [kInitStrictMode]: (attr) => assertExpectedShardsInLog(attr, 2),
        });
        assertOpenCursors(
            st,
            [st.shard0.shardName, st.shard1.shardName],
            /*expectedConfigCursor=*/ false,
            cursorCommentFilter(comment),
        );
    });

    it("unsharded collection - single shard cursor", function () {
        coll = db[collName];
        new CreateUntrackedCollectionCommand(dbName, collName, null, {exists: false}).execute(st.s);

        const comment = "strict_unsharded";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: coll,
            aggregateOptions: {comment, cursor: {batchSize: 0}},
        });

        awaitLogMessageCodes(st.s, [kInitStrictMode], () => csTest.assertNoChange(csCursor), {
            [kInitStrictMode]: (attr) => assertExpectedShardsInLog(attr, 1),
        });
        assertOpenCursors(st, [st.shard0.shardName], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));
    });

    it("collection does not exist - DbAbsent to DbPresent transition", function () {
        db.dropDatabase();
        coll = db[collName];

        const comment = "strict_db_absent";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: coll,
            aggregateOptions: {comment, cursor: {batchSize: 0}},
        });

        awaitLogMessageCodes(st.s, [kInitStrictMode], () => csTest.assertNoChange(csCursor), {
            [kInitStrictMode]: (attr) => assertExpectedShardsInLog(attr, 0),
        });
        assertOpenCursors(
            st,
            /*expectedDataShards=*/ [],
            /*expectedConfigCursor=*/ true,
            cursorCommentFilter(comment),
            st.configRS.getPrimary(),
        );

        // Create DB to trigger DbAbsent -> DbPresent transition.
        new CreateDatabaseCommand(dbName, null, null, null, st.shard2.shardName).execute(st.s);
        awaitLogMessageCodes(st.s, [kDbAbsentEvent], () => csTest.assertNoChange(csCursor), {
            [kDbAbsentEvent]: (attr) => assertExpectedShardsInLog(attr, 1),
        });
        assertOpenCursors(st, [st.shard2.shardName], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));
    });

    it("moveChunk lifecycle - partial, expansion, full drain", function () {
        coll = db[collName];

        // Manual setup: need precise chunk boundaries at -10 and 10 for the 3-step lifecycle.
        assert.commandWorked(db.createCollection(coll.getName()));
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: -10}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 10}}));

        const docs = [{_id: -11}, {_id: 0}, {_id: 11}];
        new InsertDocCommand(dbName, collName, allShards, {exists: true, shardKeySpec: {_id: 1}}, docs).execute(st.s);

        const comment = "strict_movechunk_lifecycle";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: coll,
            aggregateOptions: {comment, cursor: {batchSize: 0}},
        });

        awaitLogMessageCodes(st.s, [kInitStrictMode], () => csTest.assertNoChange(csCursor), {
            [kInitStrictMode]: (attr) =>
                assert.eq(attr.shards.length, 1, `Expected 1 shard, got ${tojsononeline(attr.shards)}`),
        });
        assertOpenCursors(st, [st.shard0.shardName], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));

        // Step 1: Move [10, MaxKey) to shard1 - partial, shard0 still has 2 chunks.
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 10},
                to: st.shard1.shardName,
                _waitForDelete: true,
            }),
        );
        awaitLogMessageCodes(st.s, [kHandleMoveChunk], () => csTest.assertNoChange(csCursor), {
            [kHandleMoveChunk]: (attr) => {
                assert.eq(attr.cursorOpenedOnRecipient, true);
                assert.eq(attr.cursorClosedOnDonor, false);
            },
        });
        assertOpenCursors(
            st,
            [st.shard0.shardName, st.shard1.shardName],
            /*expectedConfigCursor=*/ false,
            cursorCommentFilter(comment),
        );

        // Step 2: Move [-10, 10) to shard2 -- partial, shard0 still has 1 chunk. Now 3 shards.
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 0},
                to: st.shard2.shardName,
                _waitForDelete: true,
            }),
        );
        awaitLogMessageCodes(st.s, [kHandleMoveChunk], () => csTest.assertNoChange(csCursor), {
            [kHandleMoveChunk]: (attr) => {
                assert.eq(attr.cursorOpenedOnRecipient, true);
                assert.eq(attr.cursorClosedOnDonor, false);
            },
        });
        assertOpenCursors(
            st,
            [st.shard0.shardName, st.shard1.shardName, st.shard2.shardName],
            /*expectedConfigCursor=*/ false,
            cursorCommentFilter(comment),
        );

        // Step 3: Move [MinKey, -10) to shard1 -- full drain, shard0 has 0 chunks.
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: -20},
                to: st.shard1.shardName,
                _waitForDelete: true,
            }),
        );
        awaitLogMessageCodes(st.s, [kHandleMoveChunk], () => csTest.assertNoChange(csCursor), {
            [kHandleMoveChunk]: (attr) => {
                assert.eq(attr.cursorOpenedOnRecipient, false, "shard1 already open");
                assert.eq(attr.cursorClosedOnDonor, true);
            },
        });
        assertOpenCursors(
            st,
            [st.shard1.shardName, st.shard2.shardName],
            /*expectedConfigCursor=*/ false,
            cursorCommentFilter(comment),
        );
    });

    it("movePrimary for unsharded collection - placement refresh", function () {
        coll = db[collName];
        const originalShardId = st.shard0.shardName;
        const targetShardId = st.shard1.shardName;

        new CreateUntrackedCollectionCommand(dbName, collName, null, {exists: false}).execute(st.s);

        const comment = "strict_move_primary";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: coll,
            aggregateOptions: {comment, cursor: {batchSize: 0}},
        });

        awaitLogMessageCodes(st.s, [kInitStrictMode], () => csTest.assertNoChange(csCursor), {
            [kInitStrictMode]: (attr) => assertExpectedShardsInLog(attr, 1),
        });
        assertOpenCursors(st, [originalShardId], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));

        new MovePrimaryCommand(dbName, collName, null, null, targetShardId).execute(st.s);
        awaitLogMessageCodes(st.s, [kPlacementRefresh], () => csTest.assertNoChange(csCursor));
        assertOpenCursors(st, [targetShardId], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));
    });

    it("reshardCollection - cursor placement updates after NamespacePlacementChanged", function () {
        coll = db[collName];

        // Run setup commands.
        const docs = [
            {_id: -2, a: -2},
            {_id: 2, a: 2},
        ];
        [
            new ShardCollectionCommand(dbName, collName, allShards, {exists: false}, {_id: 1}),
            new InsertDocCommand(dbName, collName, null, {exists: true, shardKeySpec: {_id: 1}}, docs),
        ].forEach((cmd) => cmd.execute(st.s));
        distributeCollectionDataOverShards(db, coll, {
            middle: {_id: 0},
            chunks: [
                {find: {_id: -1}, to: st.shard0.shardName},
                {find: {_id: 1}, to: st.shard1.shardName},
            ],
        });

        const comment = "strict_reshard";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: coll,
            aggregateOptions: {comment, cursor: {batchSize: 0}},
        });

        awaitLogMessageCodes(st.s, [kInitStrictMode], () => csTest.assertNoChange(csCursor), {
            [kInitStrictMode]: (attr) => assertExpectedShardsInLog(attr, 2),
        });
        assertOpenCursors(
            st,
            [st.shard0.shardName, st.shard1.shardName],
            /*expectedConfigCursor=*/ false,
            cursorCommentFilter(comment),
        );

        const [_, shard1, shard2] = allShards;
        const targetShards = [shard1, shard2];
        const collCtx = {exists: true, shardKeySpec: {_id: 1}};
        const newShardKey = {a: 1};
        const chunkCount = 2;
        new ReshardCollectionCommand(dbName, collName, targetShards, collCtx, newShardKey, chunkCount).execute(st.s);
        awaitLogMessageCodes(st.s, [kPlacementRefresh], () => csTest.assertNoChange(csCursor));
        assertOpenCursors(
            st,
            targetShards.map((shard) => shard._id),
            /*expectedConfigCursor=*/ false,
            cursorCommentFilter(comment),
        );
    });
});
