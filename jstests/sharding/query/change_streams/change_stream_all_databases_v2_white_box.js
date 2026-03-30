/**
 * White box tests for $changeStream v2 in a sharded cluster.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_sharding,
 *   uses_change_streams,
 *   config_shard_incompatible,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it, before, afterEach, after} from "jstests/libs/mochalite.js";
import {assertCreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    ChangeStreamTest,
    ensureShardDistribution,
    awaitLogMessageCodes,
    assertOpenCursors,
} from "jstests/libs/query/change_stream_util.js";

const kInitStrictMode = 11138104; // Strict mode initialization
const kSetEventHandler = 11138113; // Set event handler
const kPlacementRefresh = 11138117; // handlePlacementRefresh (MovePrimary, NamespacePlacementChanged)

describe("$changeStream v2", function () {
    let st;
    let db;
    let adminDB;
    let coll;
    let csTest;

    before(function () {
        st = new ShardingTest({
            shards: 3,
            mongos: 1,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
            config: 1,
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

        db = st.s.getDB(jsTestName());
        coll = db.test;
        adminDB = st.s.getDB("admin");

        // Enable sharding on the the test database and ensure that the primary is shard0.
        assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    });

    afterEach(function () {
        csTest.cleanUp();
        assertDropCollection(db, coll.getName());
        db.dropDatabase();
        assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    });

    after(function () {
        st.stop();
    });

    // Helper function to create sharded collection with the given namespace and insert 2 entries.
    function createShardedCollectionWithEntries(db, coll, st) {
        assertCreateCollection(db, coll.getName());
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

        coll.insertMany([
            {_id: -1, a: -1},
            {_id: 1, a: 1},
        ]);
        ensureShardDistribution(db, coll, {
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

    it("test open cursors in an all databases change stream", function () {
        // Create and shard a collection and allocate collection to shard set {shard0, shard1}.
        createShardedCollectionWithEntries(db, coll, st);

        // Open a change stream.
        csTest = new ChangeStreamTest(adminDB);
        const csCursor = csTest.startWatchingAllChangesForCluster(
            {comment: jsTestName(), cursor: {batchSize: 0}},
            {version: "v2"},
        );
        coll.insertMany([
            {_id: 2, a: 2},
            {_id: 3, a: 3},
        ]);
        awaitLogMessageCodes(st.s, [kSetEventHandler, kInitStrictMode], () => {
            csTest.getNextChanges(csCursor, 2);
        });

        let commentFilter = {
            "cursor.originatingCommand.comment": jsTestName(),
        };
        assertOpenCursors(st, [st.shard0.shardName, st.shard1.shardName], true, commentFilter);
    });

    it("test open cursors in an all databases change stream when no databases exist", function () {
        db.dropDatabase();
        let csCursor;

        csTest = new ChangeStreamTest(adminDB);
        csCursor = csTest.startWatchingAllChangesForCluster(
            {comment: jsTestName(), cursor: {batchSize: 0}},
            {version: "v2"},
        );
        awaitLogMessageCodes(st.s, [kSetEventHandler, kInitStrictMode], () => {
            csTest.assertNoChange(csCursor);
        });

        let commentFilter = {
            "cursor.originatingCommand.comment": jsTestName(),
        };
        // TODO: SERVER-122863: no data shard cursors should be open yet as there is no data: fix expected shards to [] after the bug is fixed.
        assertOpenCursors(st, [st.shard0.shardName], true, commentFilter);

        // Create a collection and test events are handled properly.
        assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
        assertCreateCollection(db, coll.getName());
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        coll.insert([{_id: -1, a: -1}]);

        awaitLogMessageCodes(st.s, [kPlacementRefresh], () => {
            let s = csTest.getOneChange(csCursor);
        });

        // Assert cursor is open on primary data shard.
        assertOpenCursors(st, [st.shard0.shardName], true, commentFilter);
    });

    it("test open cursors in an all databases change stream with databases on different shards", function () {
        // Create unsharded collection
        assertCreateCollection(db, coll.getName());

        // Open a change stream.
        csTest = new ChangeStreamTest(adminDB);

        let csCursor = csTest.startWatchingAllChangesForCluster(
            {comment: jsTestName(), cursor: {batchSize: 0}},
            {version: "v2"},
        );

        awaitLogMessageCodes(st.s, [kSetEventHandler, kInitStrictMode], () => {
            csTest.assertNoChange(csCursor);
        });

        coll.insertMany([
            {_id: -1, a: -1},
            {_id: 1, a: 1},
        ]);

        let configConn = st.configRS.getPrimary();

        let commentFilter = {
            "cursor.originatingCommand.comment": jsTestName(),
        };
        csTest.getNextChanges(csCursor, 2);

        // Assert cursor opened on the primary data shard.
        assertOpenCursors(st, [st.shard0.shardName], true, commentFilter);

        // Create a second collection.
        const db2 = st.s.getDB(jsTestName() + "_2");
        assert.commandWorked(db.adminCommand({enableSharding: db2.getName(), primaryShard: st.shard0.shardName}));

        const coll2Name = `${jsTestName()}` + "_2";
        const coll2 = db2.getCollection(coll2Name);
        assertCreateCollection(db2, coll2.getName());
        assert.commandWorked(db2.adminCommand({shardCollection: coll2.getFullName(), key: {_id: 1}}));
        awaitLogMessageCodes(st.s, [kPlacementRefresh], () => {
            csTest.assertNoChange(csCursor);
        });

        coll2.insertMany([
            {_id: -2, a: -2},
            {_id: 2, a: 2},
        ]);

        ensureShardDistribution(db2, coll2, {
            middle: {_id: 0},
            chunks: [
                {find: {_id: -2}, to: st.shard0.shardName},
                {find: {_id: 2}, to: st.shard1.shardName},
            ],
            expectedCounts: [
                [st.shard0, 1],
                [st.shard1, 1],
                [st.shard2, 0],
            ],
        });

        csTest.getNextChanges(csCursor, 2);
        csTest.assertNoChange(csCursor);

        assert.soonNoExcept(() => {
            assertOpenCursors(st, [st.shard0.shardName, st.shard1.shardName], true, commentFilter);
            return true;
        });

        assertDropCollection(db2, coll2.getName());
    });

    it("test open cursors in an all databases change stream after moveChunk", function () {
        // Create a sharded collection
        createShardedCollectionWithEntries(db, coll, st);

        // Open a change stream.
        csTest = new ChangeStreamTest(adminDB);

        let csCursor = csTest.startWatchingAllChangesForCluster(
            {comment: jsTestName(), cursor: {batchSize: 0}},
            {version: "v2"},
        );
        awaitLogMessageCodes(st.s, [kSetEventHandler, kInitStrictMode], () => {
            csTest.assertNoChange(csCursor);
        });

        // Assert cursor opened on data shards.
        let commentFilter = {
            "cursor.originatingCommand.comment": jsTestName(),
        };
        assertOpenCursors(st, [st.shard0.shardName, st.shard1.shardName], true, commentFilter);

        // move first collection to shard1 only
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: -1},
                to: st.shard1.shardName,
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

        awaitLogMessageCodes(st.s, [kPlacementRefresh], () => {
            csTest.assertNoChange(csCursor);
        });

        coll.insert([{_id: 2, a: 2}]);
        assert.soon(() => csTest.getOneChange(csCursor), "expected change event for {_id: 2} ");

        // TODO: SERVER-122863: all chunks have been moved to shard1: fix expected shards to [st.shard1.shardName] after the bug is fixed.
        assert.soonNoExcept(() => {
            assertOpenCursors(st, [st.shard0.shardName, st.shard1.shardName], true, commentFilter);
            return true;
        });
    });

    it("test open cursors in an all databases change stream after movePrimary", function () {
        // Create unsharded collection
        assertCreateCollection(db, coll.getName());
        let commentFilter = {
            "cursor.originatingCommand.comment": jsTestName(),
        };
        // Open a change stream.
        csTest = new ChangeStreamTest(adminDB);

        const csCursor = csTest.startWatchingAllChangesForCluster(
            {comment: jsTestName(), cursor: {batchSize: 0}},
            {version: "v2"},
        );

        coll.insert([{_id: 1, a: 1}]);
        assert.soon(() => csTest.getOneChange(csCursor), "expected change event for {_id: 1} ");

        assertOpenCursors(st, [st.shard0.shardName], true, commentFilter);

        // Make shard1 the primary for the database.
        assert.commandWorked(
            st.s.adminCommand({
                movePrimary: coll.getDB().getName(),
                to: st.shard1.shardName,
            }),
        );

        awaitLogMessageCodes(st.s, [kPlacementRefresh], () => {
            csTest.assertNoChange(csCursor);
        });

        // TODO: SERVER-122863: fix expected shards to [st.shard1.shardName] after the bug is fixed.
        assertOpenCursors(st, [st.shard0.shardName, st.shard1.shardName], true, commentFilter);

        coll.insert([{_id: 2, a: 2}]);
        assert.soon(() => csTest.getOneChange(csCursor), "expected change event for {_id: 2} ");
    });

    it("test open cursors in an all databases change stream after reshard", function () {
        // Create a sharded collection
        assertCreateCollection(db, coll.getName());
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

        let configConn = st.configRS.getPrimary();
        let commentFilter = {
            "cursor.originatingCommand.comment": jsTestName(),
        };
        // Open a change stream.
        csTest = new ChangeStreamTest(adminDB);

        const csCursor = csTest.startWatchingAllChangesForCluster(
            {comment: jsTestName(), cursor: {batchSize: 0}},
            {version: "v2"},
        );

        awaitLogMessageCodes(st.s, [kSetEventHandler, kInitStrictMode], () => {
            csTest.assertNoChange(csCursor);
        });
        coll.insert([{_id: -1, a: -1}]);
        assert.soon(() => csTest.getOneChange(csCursor));

        ensureShardDistribution(db, coll, {
            middle: {_id: 0},
            chunks: [{find: {_id: -1}, to: st.shard0.shardName}],
            expectedCounts: [
                [st.shard0, 1],
                [st.shard1, 0],
                [st.shard2, 0],
            ],
        });

        assertOpenCursors(st, [st.shard0.shardName], true, commentFilter);

        coll.insert([{_id: 1, a: 1}]);
        assert.soon(() => csTest.getOneChange(csCursor));

        assert.commandWorked(
            st.s.adminCommand({
                reshardCollection: coll.getFullName(),
                key: {a: 1},
                numInitialChunks: 1,
                demoMode: true,
            }),
        );

        // TODO SERVER-122851: this test is flaky seems to have timing issue related to reshard
        // which causes the log message to sometimes be sent later.
        awaitLogMessageCodes(st.s, [kPlacementRefresh], () => {
            csTest.assertNoChange(csCursor);
        });

        coll.insert([{_id: 2, a: 2}]);
        assert.soon(() => csTest.getOneChange(csCursor), "expected change event for {a: 2} ");
        assertOpenCursors(st, [st.shard0.shardName, st.shard2.shardName], true, commentFilter);
    });
});
