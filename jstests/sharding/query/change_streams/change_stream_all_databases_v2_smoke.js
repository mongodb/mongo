/**
 * Smoke tests for $changeStream v2 in a sharded cluster.
 * Tested scenarios include:
 *  - Opening a change stream on the whole cluster, capturing events before, during, and after resharding.
 *  - Opening a change stream on the whole cluster without any existent collection, waiting for the collection to be created before returning events
 *  - Opening a change stream in the future, waiting for the start time to be reached before returning events
 *
 * @tags: [
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it, before, afterEach, after, beforeEach} from "jstests/libs/mochalite.js";
import {assertCreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

import {
    ChangeStreamTest,
    addShardToCluster,
    assertCollDataDistribution,
    ensureShardDistribution,
    getClusterTime,
} from "jstests/libs/query/change_stream_util.js";

describe("$changeStream v2", function () {
    let st;
    let db;
    let adminDb;
    let coll;
    let csTest;
    let newShard = null;

    before(function () {
        st = new ShardingTest({
            shards: 3,
            mongos: 1,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
            other: {
                enableBalancer: false,
            },
        });

        adminDb = st.s.getDB("admin");
        db = st.s.getDB(jsTestName());
        //Enable sharding on the the test database and ensure that the primary is shard0.
        assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
        coll = db.test;
        newShard = null;
    });

    afterEach(function () {
        if (csTest) {
            csTest.cleanUp();
            csTest = null;
        }
        assertDropCollection(db, coll.getName());

        if (newShard) {
            removeShard(st, newShard.shardName);
            newShard.stopSet();
        }
        newShard = null;
    });

    after(function () {
        st.stop();
    });

    it("returns events before and after resharding a collection", function () {
        // Create and shard a collection and allocate collection to shard set {shard0, shard1}.
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

        // Open a change stream.
        csTest = new ChangeStreamTest(adminDb);
        const csCursor = csTest.startWatchingAllChangesForCluster({}, {version: "v2", showExpandedEvents: true});

        // Insert documents into the collection and ensure data distribution.
        coll.insertMany([
            {_id: 2, a: 2},
            {_id: 3, a: 3},
        ]);
        assertCollDataDistribution(db, coll, [
            [st.shard0, 1],
            [st.shard1, 3],
            [st.shard2, 0],
        ]);

        // Reshard collection and allocate to shards {shard1, shard2}.
        assert.commandWorked(
            st.s.adminCommand({reshardCollection: coll.getFullName(), key: {a: 1}, numInitialChunks: 1}),
        );
        ensureShardDistribution(db, coll, {
            middle: {a: 2},
            chunks: [
                {find: {a: 1}, to: st.shard1.shardName},
                {find: {a: 2}, to: st.shard2.shardName},
            ],
            expectedCounts: [
                [st.shard0, 0],
                [st.shard1, 2],
                [st.shard2, 2],
            ],
        });

        // Insert documents into the collection and ensure data distribution.
        coll.insertMany([
            {_id: 4, a: 4},
            {_id: 5, a: 5},
        ]);
        assertCollDataDistribution(db, coll, [
            [st.shard0, 0],
            [st.shard1, 2],
            [st.shard2, 4],
        ]);

        // Drop the collection.
        assertDropCollection(db, coll.getName());

        // Read events until end of stream.
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                {
                    documentKey: {_id: 2},
                    fullDocument: {_id: 2, a: 2},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 3},
                    fullDocument: {_id: 3, a: 3},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "reshardCollection",
                },
                {
                    documentKey: {_id: 4, a: 4},
                    fullDocument: {_id: 4, a: 4},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 5, a: 5},
                    fullDocument: {_id: 5, a: 5},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "drop",
                },
            ],
        });
        csTest.assertNoChange(csCursor);
    });

    it("can be opened on a cluster without collections and returns events once a collection is created", function () {
        csTest = new ChangeStreamTest(adminDb);
        const csCursor = csTest.startWatchingAllChangesForCluster({}, {version: "v2", showExpandedEvents: true});

        // Create an unsplittable collection.
        assert.commandWorked(db.runCommand({createUnsplittableCollection: "test", dataShard: st.shard1.shardName}));

        // Insert some documents.
        coll.insertMany([
            {_id: 1, a: 1},
            {_id: 2, a: 2},
        ]);

        // Read events.
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                {
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "create",
                },
                {
                    documentKey: {_id: 1},
                    fullDocument: {_id: 1, a: 1},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 2},
                    fullDocument: {_id: 2, a: 2},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
            ],
        });

        csTest.assertNoChange(csCursor);
    });

    it("returns events after start time when opened in the future", function () {
        // Create collection.
        assertCreateCollection(db, coll.getName());

        // Open a change stream on 'coll' 3 seconds in the future.
        const testStartTime = getClusterTime(db);
        testStartTime.t += 3;
        csTest = new ChangeStreamTest(adminDb);
        let csCursor = csTest.startWatchingAllChangesForCluster(
            {},
            {version: "v2", startAtOperationTime: testStartTime, showExpandedEvents: true},
        );

        // Expect the cursor to not return any events, yet to not be closed.
        csCursor = csTest.assertNoChange(csCursor);
        assert.neq(csCursor.id, 0, "cursor was closed unexpectedly");

        // Wait until we are past the start time.
        sleep(5000);

        // Insert a document in collection.
        assert.commandWorked(coll.insert({_id: 1, a: 1}));

        // Read event.
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                {
                    documentKey: {_id: 1},
                    fullDocument: {_id: 1, a: 1},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
            ],
        });
        csTest.assertNoChange(csCursor);
    });

    it("returns events from multiple collections", function () {
        // Create and shard a collection and allocate collection to shard set {shard0, shard1}.
        const coll2Name = `${jsTestName()}` + "_2";
        const coll2 = db.getCollection(coll2Name);

        assertCreateCollection(db, coll.getName());
        assertCreateCollection(db, coll2.getName());

        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({shardCollection: coll2.getFullName(), key: {_id: 1}}));
        coll.insertMany([
            {_id: -1, a: -1},
            {_id: 1, a: 1},
        ]);

        coll2.insertMany([
            {_id: -2, a: -2},
            {_id: 2, a: 2},
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

        ensureShardDistribution(db, coll2, {
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
        // // Open a change stream.
        csTest = new ChangeStreamTest(adminDb);
        const csCursor = csTest.startWatchingAllChangesForCluster({}, {version: "v2", showExpandedEvents: true});

        // Insert documents into the collection and ensure data distribution.
        coll.insertMany([
            {_id: 2, a: 2},
            {_id: 3, a: 3},
        ]);
        coll2.insertMany([
            {_id: 4, a: 4},
            {_id: 5, a: 5},
        ]);
        assertCollDataDistribution(db, coll, [
            [st.shard0, 1],
            [st.shard1, 3],
            [st.shard2, 0],
        ]);

        assertCollDataDistribution(db, coll2, [
            [st.shard0, 1],
            [st.shard1, 3],
            [st.shard2, 0],
        ]);

        // Reshard collection and allocate to shards {shard1, shard2}.
        assert.commandWorked(
            st.s.adminCommand({reshardCollection: coll.getFullName(), key: {a: 1}, numInitialChunks: 1}),
        );
        ensureShardDistribution(db, coll, {
            middle: {a: 2},
            chunks: [
                {find: {a: 1}, to: st.shard1.shardName},
                {find: {a: 2}, to: st.shard2.shardName},
            ],
            expectedCounts: [
                [st.shard0, 0],
                [st.shard1, 2],
                [st.shard2, 2],
            ],
        });

        // Reshard second collection and allocate to shards {shard1, shard2}.

        assert.commandWorked(
            st.s.adminCommand({reshardCollection: coll2.getFullName(), key: {a: 1}, numInitialChunks: 1}),
        );
        ensureShardDistribution(db, coll2, {
            middle: {a: 3},
            chunks: [
                {find: {a: -2}, to: st.shard1.shardName},
                {find: {a: 5}, to: st.shard2.shardName},
            ],
            expectedCounts: [
                [st.shard0, 0],
                [st.shard1, 2],
                [st.shard2, 2],
            ],
        });

        // Insert documents into the collection and ensure data distribution.
        coll.insertMany([
            {_id: 4, a: 4},
            {_id: 5, a: 5},
        ]);
        assertCollDataDistribution(db, coll, [
            [st.shard0, 0],
            [st.shard1, 2],
            [st.shard2, 4],
        ]);

        // Drop the collection.
        assertDropCollection(db, coll.getName());
        assertDropCollection(db, coll2.getName());

        // Read events until end of stream.
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                {
                    documentKey: {_id: 2},
                    fullDocument: {_id: 2, a: 2},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 3},
                    fullDocument: {_id: 3, a: 3},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 4},
                    fullDocument: {_id: 4, a: 4},
                    ns: {db: db.getName(), coll: coll2.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 5},
                    fullDocument: {_id: 5, a: 5},
                    ns: {db: db.getName(), coll: coll2.getName()},
                    operationType: "insert",
                },
                {
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "reshardCollection",
                },
                {
                    ns: {db: db.getName(), coll: coll2.getName()},
                    operationType: "reshardCollection",
                },
                {
                    documentKey: {_id: 4, a: 4},
                    fullDocument: {_id: 4, a: 4},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 5, a: 5},
                    fullDocument: {_id: 5, a: 5},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "drop",
                },
                {
                    ns: {db: db.getName(), coll: coll2.getName()},
                    operationType: "drop",
                },
            ],
        });

        csTest.assertNoChange(csCursor);
    });

    it("returns events from multiple databases", function () {
        // Create and shard a collection and allocate collection to shard set {shard0, shard1}.
        const db2 = st.s.getDB(jsTestName() + "_2");
        const coll2Name = `${jsTestName()}` + "_2";
        const coll2 = db2.getCollection(coll2Name);

        assertCreateCollection(db, coll.getName());
        assertCreateCollection(db2, coll2.getName());

        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db2.adminCommand({shardCollection: coll2.getFullName(), key: {_id: 1}}));

        // // Open a change stream.
        csTest = new ChangeStreamTest(adminDb);
        const csCursor = csTest.startWatchingAllChangesForCluster({}, {version: "v2", showExpandedEvents: true});

        coll.insertMany([
            {_id: -1, a: -1},
            {_id: 1, a: 1},
        ]);

        coll2.insertMany([
            {_id: -2, a: -2},
            {_id: 2, a: 2},
        ]);

        // Drop the collections.
        assertDropCollection(db, coll.getName());
        assertDropCollection(db2, coll2.getName());

        // Read events until end of stream.
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                {
                    documentKey: {_id: -1},
                    fullDocument: {_id: -1, a: -1},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 1},
                    fullDocument: {_id: 1, a: 1},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: -2},
                    fullDocument: {_id: -2, a: -2},
                    ns: {db: db2.getName(), coll: coll2.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 2},
                    fullDocument: {_id: 2, a: 2},
                    ns: {db: db2.getName(), coll: coll2.getName()},
                    operationType: "insert",
                },
                {
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "drop",
                },
                {
                    ns: {db: db2.getName(), coll: coll2.getName()},
                    operationType: "drop",
                },
            ],
        });

        csTest.assertNoChange(csCursor);
    });

    it("returns events when new shards are added and a new collection is created", function () {
        // Open a change stream.
        csTest = new ChangeStreamTest(adminDb);
        const csCursor = csTest.startWatchingAllChangesForCluster({}, {version: "v2", showExpandedEvents: true});

        const rsNodeOptions = {
            setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1},
        };
        const newShardName = "newShard1";
        newShard = addShardToCluster(st, newShardName, 1, rsNodeOptions);
        newShard.shardName = newShardName;

        // Create and shard a collection and allocate collection to shard set {shard0, shard1}.
        assertCreateCollection(db, coll.getName());

        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        coll.insertMany([
            {_id: -1, a: -1},
            {_id: 1, a: 1},
            {_id: 2, a: 2},
            {_id: 3, a: 3},
        ]);

        ensureShardDistribution(db, coll, {
            middle: {_id: 0},
            chunks: [
                {find: {_id: -1}, to: st.shard2.shardName},
                {find: {_id: 1}, to: newShard.shardName},
            ],
            expectedCounts: [
                [st.shard0, 0],
                [st.shard1, 0],
                [st.shard2, 1],
                [newShard.getPrimary(), 3],
            ],
        });

        coll.insert({_id: 4, a: 4});
        assertCollDataDistribution(db, coll, [
            [st.shard0, 0],
            [st.shard1, 0],
            [st.shard2, 1],
            [newShard.getPrimary(), 4],
        ]);

        // Drop the collections.
        assertDropCollection(db, coll.getName());

        // Read events until end of stream.
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                {
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "create",
                },
                {
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "shardCollection",
                },
                {
                    documentKey: {_id: -1},
                    fullDocument: {_id: -1, a: -1},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 1},
                    fullDocument: {_id: 1, a: 1},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 2},
                    fullDocument: {_id: 2, a: 2},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 3},
                    fullDocument: {_id: 3, a: 3},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 4},
                    fullDocument: {_id: 4, a: 4},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "drop",
                },
            ],
        });

        csTest.assertNoChange(csCursor);
    });

    it("returns events when new shards are added and collection is resharded", function () {
        // Create and shard a collection and allocate collection to shard set {shard0, shard1}.
        assertCreateCollection(db, coll.getName());

        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        coll.insertMany([
            {_id: -1, a: -1},
            {_id: 1, a: 1},
            {_id: 2, a: 2},
            {_id: 3, a: 3},
        ]);

        assert.commandWorked(
            st.s.adminCommand({reshardCollection: coll.getFullName(), key: {a: 1}, numInitialChunks: 1}),
        );
        ensureShardDistribution(db, coll, {
            middle: {a: 2},
            chunks: [
                {find: {a: -1}, to: st.shard1.shardName},
                {find: {a: 3}, to: st.shard2.shardName},
            ],
            expectedCounts: [
                [st.shard0, 0],
                [st.shard1, 2],
                [st.shard2, 2],
            ],
        });

        // Open a change stream.
        csTest = new ChangeStreamTest(adminDb);
        const csCursor = csTest.startWatchingAllChangesForCluster({}, {version: "v2", showExpandedEvents: true});

        const rsNodeOptions = {
            setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1},
        };
        const newShardName = "newShard1";
        newShard = addShardToCluster(st, newShardName, 1, rsNodeOptions);
        newShard.shardName = newShardName;

        assert.commandWorked(
            st.s.adminCommand({reshardCollection: coll.getFullName(), key: {a: 1}, numInitialChunks: 1}),
        );
        ensureShardDistribution(db, coll, {
            middle: {a: 2},
            chunks: [
                {find: {a: 1}, to: st.shard1.shardName},
                {find: {a: 2}, to: newShard.shardName},
            ],
            expectedCounts: [
                [st.shard0, 0],
                [st.shard1, 2],
                [newShard.getPrimary(), 2],
            ],
        });

        coll.insertMany([
            {_id: 4, a: 4},
            {_id: 5, a: 5},
        ]);
        // Drop the collections.
        assertDropCollection(db, coll.getName());

        // Read events until end of stream.
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                {
                    documentKey: {_id: 4, a: 4},
                    fullDocument: {_id: 4, a: 4},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    documentKey: {_id: 5, a: 5},
                    fullDocument: {_id: 5, a: 5},
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "insert",
                },
                {
                    ns: {db: db.getName(), coll: coll.getName()},
                    operationType: "drop",
                },
            ],
        });

        csTest.assertNoChange(csCursor);
    });
});
