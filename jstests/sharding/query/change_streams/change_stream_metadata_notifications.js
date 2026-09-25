// Tests metadata notifications of change streams on sharded collections.
// @tags: [
//   # A whole-cluster stream never invalidates on a database drop (only collection-scoped and
//   # whole-db streams do), so there is nothing to observe here under whole-cluster scope.
//   do_not_run_in_whole_cluster_passthrough,
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
import {describe, it, before, after} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    assertChangeStreamEventEq,
    assertInvalidateOp,
    ChangeStreamTest,
    isChangeStreamPassthrough,
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("change stream metadata notifications on sharded collections", function () {
    let st, mongosDB, mongosColl;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });
        mongosDB = st.s0.getDB(jsTestName());
        mongosColl = mongosDB[jsTestName()];

        // Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
        assert.commandWorked(
            mongosDB.adminCommand({
                enableSharding: mongosDB.getName(),
                primaryShard: st.rs0.getURL(),
            }),
        );

        // Shard the test collection on a field called 'shardKey'.
        assert.commandWorked(
            mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {shardKey: 1}}),
        );

        // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
        assert.commandWorked(
            mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {shardKey: 0}}),
        );

        // Move the [0, MaxKey] chunk to st.shard1.shardName.
        assert.commandWorked(
            mongosDB.adminCommand({
                moveChunk: mongosColl.getFullName(),
                find: {shardKey: 1},
                to: st.rs1.getURL(),
            }),
        );
    });

    after(function () {
        st.stop();
    });

    // A whole-db/whole-cluster stream doesn't invalidate on a single collection's drop, so every
    // assertion below: the drop/invalidate sequence itself, resuming from the invalidate token,
    // and tolerating the collection being recreated with a new UUID only applies at collection scope.
    if (!isChangeStreamPassthrough()) {
        describe("collection drop", function () {
            let resumeTokenFromFirstUpdate, collectionDropInvalidateToken;

            before(function () {
                // Write a document to each chunk before opening the change stream, so that the
                // change stream shouldn't return them.
                assert.commandWorked(
                    mongosColl.insert({shardKey: -1, _id: -1}, {writeConcern: {w: "majority"}}),
                );
                assert.commandWorked(
                    mongosColl.insert({shardKey: 1, _id: 1}, {writeConcern: {w: "majority"}}),
                );

                const cst = new ChangeStreamTest(mongosDB);
                const changeStream = cst.startWatchingChanges({
                    pipeline: [{$changeStream: {}}],
                    collection: mongosColl.getName(),
                });

                assert.commandWorked(
                    mongosColl.update({shardKey: -1, _id: -1}, {$set: {updated: true}}),
                );
                assert.commandWorked(
                    mongosColl.update({shardKey: 1, _id: 1}, {$set: {updated: true}}),
                );
                assert.commandWorked(mongosColl.insert({shardKey: 2, _id: 2}));

                // Drop the collection and verify we see the two updates, the insert, then a drop
                // and an invalidate. resumeTokenFromFirstUpdate and collectionDropInvalidateToken
                // are needed by the it()s below.
                mongosColl.drop();

                const ns = {db: mongosDB.getName(), coll: mongosColl.getName()};
                const changes = cst.assertNextChangesEqual({
                    cursor: changeStream,
                    expectedChanges: [
                        {operationType: "update", ns, documentKey: {shardKey: -1, _id: -1}},
                        {operationType: "update", ns, documentKey: {shardKey: 1, _id: 1}},
                        {
                            operationType: "insert",
                            ns,
                            documentKey: {shardKey: 2, _id: 2},
                            fullDocument: {shardKey: 2, _id: 2},
                        },
                        {operationType: "drop", ns},
                        {operationType: "invalidate"},
                    ],
                    expectInvalidate: true,
                });
                resumeTokenFromFirstUpdate = changes[0]._id;
                collectionDropInvalidateToken = changes[changes.length - 1]._id;
            });

            it("still returns the invalidate token when resuming with a filter that matches nothing", function () {
                const resumeStream = mongosColl.watch(
                    [{$match: {operationType: "DummyOperationType"}}],
                    {
                        resumeAfter: resumeTokenFromFirstUpdate,
                    },
                );
                assert.soon(() => {
                    assert(!resumeStream.hasNext());
                    return resumeStream.isExhausted();
                });
                assert.eq(resumeStream.getResumeToken(), collectionDropInvalidateToken);
            });

            it("resumes from before the drop with an explicit collation and observes the same drop/invalidate sequence", function () {
                const cst = new ChangeStreamTest(mongosDB);
                const resumeWithCollationStream = cst.startWatchingChanges({
                    pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
                    collection: mongosColl.getName(),
                    aggregateOptions: {collation: {locale: "simple"}},
                });

                const ns = {db: mongosDB.getName(), coll: mongosColl.getName()};
                cst.assertNextChangesEqual({
                    cursor: resumeWithCollationStream,
                    expectedChanges: [
                        {operationType: "update", ns, documentKey: {shardKey: 1, _id: 1}},
                        {
                            operationType: "insert",
                            ns,
                            documentKey: {shardKey: 2, _id: 2},
                            fullDocument: {shardKey: 2, _id: 2},
                        },
                        {operationType: "drop", ns},
                        {operationType: "invalidate"},
                    ],
                    expectInvalidate: true,
                });
            });

            it("resumes from before the drop without specifying an explicit collation", function () {
                // Test that we can resume the change stream without specifying an explicit collation.
                assert.commandWorked(
                    mongosDB.runCommand({
                        aggregate: mongosColl.getName(),
                        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
                        cursor: {},
                    }),
                );
            });

            it("tolerates resuming after the collection is recreated with a new UUID", function () {
                // Recreate and shard the collection.
                assert.commandWorked(mongosDB.createCollection(mongosColl.getName()));
                assert.commandWorked(
                    mongosDB.adminCommand({
                        shardCollection: mongosColl.getFullName(),
                        key: {shardKey: 1},
                    }),
                );

                // Test that resuming the change stream on the recreated collection succeeds, since we
                // will not attempt to inherit the collection's default collation and can therefore
                // ignore the new UUID.
                assert.commandWorked(
                    mongosDB.runCommand({
                        aggregate: mongosColl.getName(),
                        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
                        cursor: {},
                    }),
                );
            });
        });
    }

    describe("database drop", function () {
        let resumeTokenAfterDbDrop, dbDropInvalidateToken;

        before(function () {
            // Recreate the collection as unsharded.
            assertDropAndRecreateCollection(mongosDB, mongosColl.getName());

            const changeStream = mongosColl.watch();

            // Drop the database and verify that the stream returns a collection drop followed by
            // an invalidate. resumeTokenAfterDbDrop and dbDropInvalidateToken are needed by the
            // it() below.
            assert.commandWorked(mongosDB.dropDatabase());

            assert.soon(() => changeStream.hasNext());
            const next = changeStream.next();
            assertChangeStreamEventEq(next, {
                operationType: "drop",
                ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
            });
            resumeTokenAfterDbDrop = next._id;

            const invalidate = assertInvalidateOp({cursor: changeStream, opType: "dropDatabase"});
            dbDropInvalidateToken = invalidate._id;
        });

        it("still returns the invalidate token when resuming with a filter that matches nothing", function () {
            const resumeStream = mongosColl.watch(
                [{$match: {operationType: "DummyOperationType"}}],
                {
                    resumeAfter: resumeTokenAfterDbDrop,
                },
            );
            assert.soon(() => {
                assert(!resumeStream.hasNext());
                return resumeStream.isExhausted();
            });
            assert.eq(resumeStream.getResumeToken(), dbDropInvalidateToken);
        });
    });
});
