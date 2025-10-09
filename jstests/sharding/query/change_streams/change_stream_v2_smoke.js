/**
 * Smoke tests for $changeStream v2 in a sharded cluster.
 * Tested scenarios include:
 *  - Opening a change stream on an existing collection, capturing events before, during, and after resharding, until invalidation
 *  - Opening a change stream on a non-existent collection, waiting for the collection to be created before returning events
 *  - Opening a change stream in the future, waiting for the start time to be reached before returning events
 *
 * @tags: [
 *   featureFlagChangeStreamPreciseShardTargeting,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it, before, afterEach, after, beforeEach} from "jstests/libs/mochalite.js";
import {assertCreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

describe("$changeStream v2", function () {
    let st;
    let db;
    let coll;
    let csTest;

    before(function () {
        st = new ShardingTest({
            shards: 3,
            mongos: 1,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
            other: {
                enableBalancer: false,
            },
        });

        db = st.s.getDB(jsTestName());
        coll = db.test;

        //Enable sharding on the the test database and ensure that the primary is shard0.
        assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    });

    afterEach(function () {
        csTest.cleanUp();
        assertDropCollection(db, coll.getName());
    });

    after(function () {
        st.stop();
    });

    function assertCollDataDistribution(expectedCounts) {
        for (const [shardConn, expectedCount] of expectedCounts) {
            assert.soon(
                () => {
                    const docs = shardConn.getDB(db.getName())[coll.getName()].find().toArray();
                    return expectedCount == docs.length;
                },
                "Expected " + expectedCount + " documents on " + shardConn,
            );
        }
    }

    function distributeCollDataOverShards(coll, distributionConfig) {
        assert.commandWorked(
            st.s.adminCommand({
                split: coll.getFullName(),
                middle: distributionConfig.middle,
            }),
        );
        for (const chunkConfig of distributionConfig.chunks) {
            assert.commandWorked(
                st.s.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: chunkConfig.find,
                    to: chunkConfig.to,
                    _waitForDelete: true,
                }),
            );
        }
        assertCollDataDistribution(distributionConfig.expectedCounts);
    }

    it("returns events before and after resharding until invalidation", function () {
        // Create and shard a collection and allocate collection to shard set {shard0, shard1}.
        assertCreateCollection(db, coll.getName());
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        coll.insertMany([
            {_id: -1, a: -1},
            {_id: 1, a: 1},
        ]);
        distributeCollDataOverShards(coll, {
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
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({pipeline: [{$changeStream: {version: "v2"}}], collection: coll});

        // Insert documents into the collection and ensure data distribution.
        coll.insertMany([
            {_id: 2, a: 2},
            {_id: 3, a: 3},
        ]);
        assertCollDataDistribution([
            [st.shard0, 1],
            [st.shard1, 3],
            [st.shard2, 0],
        ]);

        // Reshard collection and allocate to shards {shard1, shard2}.
        assert.commandWorked(
            st.s.adminCommand({reshardCollection: coll.getFullName(), key: {a: 1}, numInitialChunks: 1}),
        );
        distributeCollDataOverShards(coll, {
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
        assertCollDataDistribution([
            [st.shard0, 0],
            [st.shard1, 2],
            [st.shard2, 4],
        ]);

        // Drop the collection.
        assertDropCollection(db, coll.getName());

        // Read events until invalidation.
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
                    operationType: "invalidate",
                },
            ],
        });
    });

    it("can be opened on non-existing collection and returns events once it is created", function () {
        // Open the change stream just after the invalidation cluster time.
        csTest = new ChangeStreamTest(db);
        const csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2", showExpandedEvents: true}}],
            collection: coll,
        });

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
    });

    it("returns events after start time when opened in the future", function () {
        // Create collection.
        assertCreateCollection(db, coll.getName());

        // Open a change stream on 'coll' 3 seconds in the future.
        const testStartTime = db.adminCommand({hello: 1}).$clusterTime.clusterTime;
        testStartTime.t += 3;
        csTest = new ChangeStreamTest(db);
        let csCursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2", startAtOperationTime: testStartTime}}],
            collection: coll,
        });

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
    });
});
