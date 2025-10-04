// Tests the behavior of change streams on a collection that was initially unsharded but then
// becomes sharded. In particular, test that post-shardCollection inserts update their cached
// 'documentKey' to include the new shard key, and that a resume token obtained prior to the
// shardCollection command can be used to resume the stream even after the collection has been
// sharded.
// @tags: [
//   # TODO SERVER-30784: Remove 'multiversion_incompatible' tag and
//   # 'throwChangeStreamTopologyChangeExceptionToClient'.
//   multiversion_incompatible,
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const testName = "change_streams_unsharded_becomes_sharded";
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {
        nodes: 1,
        // Use a higher frequency for periodic noops to speed up the test.
        setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true},
    },
});

assert.commandWorked(st.s.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName}));
const mongosDB = st.s0.getDB("test");
const mongosColl = mongosDB[testName];

function testUnshardedBecomesSharded(collToWatch) {
    mongosColl.drop();
    mongosDB.createCollection(testName);
    mongosColl.createIndex({x: 1});

    // Establish a change stream cursor on the unsharded collection.
    const cst = new ChangeStreamTest(mongosDB);

    // Create a different collection in the same database, and verify that it doesn't affect the
    // results of the change stream.
    const mongosCollOther = mongosDB[testName + "other"];
    mongosCollOther.drop();
    mongosDB.createCollection(testName + "other");
    mongosCollOther.createIndex({y: 1});

    let cursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {}}, {$match: {"ns.coll": mongosColl.getName()}}],
        collection: collToWatch,
    });
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Verify that the cursor picks up documents inserted while the collection is unsharded. The
    // 'documentKey' at this point is simply the _id field.
    assert.commandWorked(mongosColl.insert({_id: 0, x: 0}));
    assert.commandWorked(mongosCollOther.insert({_id: 0, y: 0}));
    const [preShardCollectionChange] = cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [
            {
                documentKey: {_id: 0},
                fullDocument: {_id: 0, x: 0},
                ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
                operationType: "insert",
            },
        ],
    });

    // Record the resume token for this change, before the collection is sharded.
    const preShardCollectionResumeToken = preShardCollectionChange._id;

    // Shard the test collection with shard key {x: 1} and split into 2 chunks.
    st.shardColl(mongosColl.getName(), {x: 1}, {x: 0}, false, mongosDB.getName());

    // Shard the other collection with shard key {y: 1} and split into 2 chunks.
    st.shardColl(mongosCollOther.getName(), {y: 1}, {y: 0}, false, mongosDB.getName());

    // List the changes we expect to see for the next two operations on the sharded collection.
    // Later, we will resume the stream using the token generated before the collection was
    // sharded, and will need to confirm that we can still see these two changes.
    const postShardCollectionChanges = [
        {
            documentKey: {x: 1, _id: 1},
            fullDocument: {_id: 1, x: 1},
            ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
            operationType: "insert",
        },
        {
            documentKey: {x: -1, _id: -1},
            fullDocument: {_id: -1, x: -1},
            ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
            operationType: "insert",
        },
    ];

    // Verify that the cursor on the original shard is still valid and sees new inserted
    // documents. The 'documentKey' field should now include the shard key.
    assert.commandWorked(mongosColl.insert({_id: 1, x: 1}));
    assert.commandWorked(mongosCollOther.insert({_id: 1, y: 1}));
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [postShardCollectionChanges[0]]});

    // Move the [minKey, 0) chunk to shard1.
    assert.commandWorked(
        mongosDB.adminCommand({
            moveChunk: mongosColl.getFullName(),
            find: {x: -1},
            to: st.rs1.getURL(),
            _waitForDelete: true,
        }),
    );
    assert.commandWorked(
        mongosDB.adminCommand({
            moveChunk: mongosCollOther.getFullName(),
            find: {y: -1},
            to: st.rs1.getURL(),
            _waitForDelete: true,
        }),
    );

    // Make sure the change stream cursor sees a document inserted on the recipient shard.
    assert.commandWorked(mongosColl.insert({_id: -1, x: -1}));
    assert.commandWorked(mongosCollOther.insert({_id: -1, y: -1}));
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [postShardCollectionChanges[1]]});

    // Confirm that we can resume the stream on the sharded collection using the token generated
    // while the collection was unsharded, whose documentKey contains the _id field but not the
    // shard key.
    let resumedCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: preShardCollectionResumeToken}}],
        collection: mongosColl,
    });

    // Verify that we see both of the insertions which occurred after the collection was
    // sharded.
    cst.assertNextChangesEqual({cursor: resumedCursor, expectedChanges: postShardCollectionChanges});

    // Test the behavior of a change stream when a sharded collection is dropped and recreated.
    cursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {}}, {$match: {"ns.coll": mongosColl.getName()}}],
        collection: collToWatch,
    });
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Insert a couple documents to shard1, creating a scenario where the getMore to shard0 will
    // indicate that the change stream is invalidated yet shard1 will still have data to return.
    assert.commandWorked(mongosColl.insert({_id: -2, x: -2}));
    assert.commandWorked(mongosColl.insert({_id: -3, x: -3}));

    // Drop and recreate the collection.
    mongosColl.drop();
    mongosDB.createCollection(mongosColl.getName());
    mongosColl.createIndex({z: 1});

    // Shard the collection on a different shard key and ensure that each shard has a chunk.
    st.shardColl(mongosColl.getName(), {z: 1}, {z: 0}, {z: -1}, mongosDB.getName());

    assert.commandWorked(mongosColl.insert({_id: -1, z: -1}));
    assert.commandWorked(mongosColl.insert({_id: 1, z: 1}));

    // Verify that the change stream picks up the inserts. The shard keys are present since they are
    // recorded in the oplog.
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [
            {
                documentKey: {x: -2, _id: -2},
                fullDocument: {_id: -2, x: -2},
                ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
                operationType: "insert",
            },
            {
                documentKey: {x: -3, _id: -3},
                fullDocument: {_id: -3, x: -3},
                ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
                operationType: "insert",
            },
        ],
    });

    // Verify that the kNewShardDetected event is successfully delivered to mongoS even in cases
    // where the event does not match the user's filter.
    // TODO SERVER-30784: remove this test-case, or rework it without the failpoint, when the
    // kNewShardDetected event is the only way we detect a new shard for the collection.
    mongosDB.adminCommand({configureFailPoint: "throwChangeStreamTopologyChangeExceptionToClient", mode: "alwaysOn"});
    ChangeStreamTest.assertChangeStreamThrowsCode({
        db: mongosDB,
        collName: collToWatch,
        pipeline: [{$changeStream: {resumeAfter: preShardCollectionResumeToken}}, {$match: {operationType: "delete"}}],
        expectedCode: ErrorCodes.ChangeStreamTopologyChange,
    });
    mongosDB.adminCommand({configureFailPoint: "throwChangeStreamTopologyChangeExceptionToClient", mode: "off"});

    cst.cleanUp();
}

// First test against a change stream on a single collection.
testUnshardedBecomesSharded(mongosColl.getName());

// Test against a change stream on the entire database.
testUnshardedBecomesSharded(1);

st.stop();
