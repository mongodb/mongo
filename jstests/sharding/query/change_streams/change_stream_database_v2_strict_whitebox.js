/**
 * White-box integration tests for database-level change stream v2 shard targeting in strict mode.
 * Verifies observable shard-targeting behavior: which shards have open cursors after each lifecycle
 * event, and that placement history is consulted at the right times.
 *
 * Unlike the collection-level targeter, the database targeter delegates handleMoveChunk() to
 * handlePlacementRefresh() (full re-evaluation), so moveChunk events produce kPlacementRefresh
 * logs instead of kHandleMoveChunk.
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
    V2TargeterLogCodes,
} from "jstests/libs/query/change_stream_util.js";
import {
    CreateDatabaseCommand,
    CreateUntrackedCollectionCommand,
    InsertDocCommand,
    ShardCollectionCommand,
    ReshardCollectionCommand,
    MovePrimaryCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";

const {
    kCollOrDbShardTargeterInitStrictMode: kInitStrictMode,
    kCollOrDbPlacementRefresh: kPlacementRefresh,
    kCollOrDbDbAbsentEventHandling: kDbAbsentEvent,
} = V2TargeterLogCodes;

describe("database v2 strict whitebox", function () {
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

        const comment = "db_strict_initial_placement";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: 1,
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

        const comment = "db_strict_unsharded";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: 1,
            aggregateOptions: {comment, cursor: {batchSize: 0}},
        });

        awaitLogMessageCodes(st.s, [kInitStrictMode], () => csTest.assertNoChange(csCursor), {
            [kInitStrictMode]: (attr) => assertExpectedShardsInLog(attr, 1),
        });
        assertOpenCursors(st, [st.shard0.shardName], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));
    });

    it("database does not exist - DbAbsent to DbPresent transition", function () {
        db.dropDatabase();
        coll = db[collName];

        const comment = "db_strict_db_absent";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: 1,
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

    it("moveChunk lifecycle - placement refresh at each step", function () {
        coll = db[collName];

        // Manual setup: need precise chunk boundaries at -10 and 10 for the 3-step lifecycle.
        assert.commandWorked(db.createCollection(coll.getName()));
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: -10}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 10}}));

        const docs = [{_id: -11}, {_id: 0}, {_id: 11}];
        new InsertDocCommand(dbName, collName, allShards, {exists: true, shardKeySpec: {_id: 1}}, docs).execute(st.s);

        const comment = "db_strict_movechunk_lifecycle";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: 1,
            aggregateOptions: {comment, cursor: {batchSize: 0}},
        });

        awaitLogMessageCodes(st.s, [kInitStrictMode], () => csTest.assertNoChange(csCursor), {
            [kInitStrictMode]: (attr) =>
                assert.eq(attr.shards.length, 1, `Expected 1 shard, got ${tojsononeline(attr.shards)}`),
        });
        assertOpenCursors(st, [st.shard0.shardName], /*expectedConfigCursor=*/ false, cursorCommentFilter(comment));

        // Step 1: Move [10, MaxKey) to shard1 - placement refresh re-evaluates, cursors on shard0 + shard1.
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 10},
                to: st.shard1.shardName,
                _waitForDelete: true,
            }),
        );
        awaitLogMessageCodes(st.s, [kPlacementRefresh], () => csTest.assertNoChange(csCursor));
        assertOpenCursors(
            st,
            [st.shard0.shardName, st.shard1.shardName],
            /*expectedConfigCursor=*/ false,
            cursorCommentFilter(comment),
        );

        // Step 2: Move [-10, 10) to shard2 - placement refresh, now 3 shards.
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 0},
                to: st.shard2.shardName,
                _waitForDelete: true,
            }),
        );
        awaitLogMessageCodes(st.s, [kPlacementRefresh], () => csTest.assertNoChange(csCursor));
        assertOpenCursors(
            st,
            [st.shard0.shardName, st.shard1.shardName, st.shard2.shardName],
            /*expectedConfigCursor=*/ false,
            cursorCommentFilter(comment),
        );

        // Step 3: Move [MinKey, -10) to shard1 - all chunks off shard0, but shard0 is still
        // the DB primary so the database targeter keeps a cursor there.
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: -20},
                to: st.shard1.shardName,
                _waitForDelete: true,
            }),
        );
        awaitLogMessageCodes(st.s, [kPlacementRefresh], () => csTest.assertNoChange(csCursor));
        assertOpenCursors(
            st,
            [st.shard0.shardName, st.shard1.shardName, st.shard2.shardName],
            /*expectedConfigCursor=*/ false,
            cursorCommentFilter(comment),
        );
    });

    it("movePrimary for unsharded collection - placement refresh", function () {
        coll = db[collName];
        const originalShardId = st.shard0.shardName;
        const targetShardId = st.shard1.shardName;

        new CreateUntrackedCollectionCommand(dbName, collName, null, {exists: false}).execute(st.s);

        const comment = "db_strict_move_primary";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: 1,
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

        const comment = "db_strict_reshard";
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: 1,
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
        // shard0 is still the DB primary, so the database targeter keeps a cursor there
        // even though no collection chunks reside on it after resharding.
        assertOpenCursors(
            st,
            allShards.map((shard) => shard._id),
            /*expectedConfigCursor=*/ false,
            cursorCommentFilter(comment),
        );
    });
});
