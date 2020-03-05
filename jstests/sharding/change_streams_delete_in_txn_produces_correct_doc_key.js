// Tests that 'delete' events within a multi-document transaction do not include the full document
// but only the shard key and _id in the 'documentKey' field. Exercises the fix for SERVER-45987.
// @tags: [uses_transactions, multiversion_incompatible]

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.

// Create a new sharded cluster. For this test, in addition to testing 'delete' events within a
// multi-document transaction, we only test the 'documentKey' field contains the shard key in a
// sharded cluster, so we only need a single shard.
const s = new ShardingTest({shards: 1});
const dbName = "test";
const collName = "change_streams_delete_in_txn_produces_correct_doc_key";
const ns = dbName + "." + collName;
const db = s.getDB(dbName);

/**
 * Test function which is used to test three types of delete-related commands, deleteOne(),
 * deleteMany() and findAndModify(). Ensure only documentKey instead of a full document will be
 * logged on entries for deletes in multi-document transactions, and also ensure that we can resume
 * the change stream from these delete events.
 */
function testDeleteInMultiDocTxn({collName, deleteCommand, expectedChanges}) {
    // Initialize the collection.
    const coll = assertDropAndRecreateCollection(db, collName);
    // Enable sharding on DB.
    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    // Shard the collection using field 'a' as the shard key.
    assert.commandWorked(db.adminCommand({shardCollection: ns, key: {a: 1, _id: 1}}));

    assert.commandWorked(coll.insertMany([
        {_id: 1, a: 0, fullDoc: "It's a full document!"},
        {_id: 2, a: 0},
        {_id: 3, a: 0},
        {_id: 4, a: 1}
    ]));

    // Open a change stream on the test collection.
    const cst = new ChangeStreamTest(db);
    let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: coll});

    // Start a transaction in which to perform the delete operation(s).
    const session = db.getMongo().startSession();
    const sessionDb = session.getDatabase(db.getName());
    const sessionColl = sessionDb[collName];
    session.startTransaction();

    // Run the given 'deleteCommand' function to perform the delete(s).
    deleteCommand(sessionColl);

    // Commit the transaction so that the events become visible to the change stream.
    assert.commandWorked(session.commitTransaction_forTesting());

    // Verify that the stream returns the expected sequence of changes.
    const changes = cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expectedChanges});

    // Test the change stream can be resumed after a delete event from within the transaction.
    cursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: changes[changes.length - 1]._id}}],
        collection: coll
    });
    assert.commandWorked(coll.insert({a: 1, _id: 5}));
    assert.docEq(cst.getOneChange(cursor).documentKey, {a: 1, _id: 5});

    cst.cleanUp();
}

jsTestLog("Testing deleteOne() in a transaction.");
testDeleteInMultiDocTxn({
    collName: collName,
    deleteCommand: function(sessionColl) {
        assert.commandWorked(sessionColl.deleteOne({_id: 1}));
    },
    expectedChanges: [
        {
            documentKey: {a: 0, _id: 1},
            ns: {db: db.getName(), coll: collName},
            operationType: "delete",
        },
    ],
});

jsTestLog("Testing deleteMany() in a transaction.");
testDeleteInMultiDocTxn({
    collName: collName,
    deleteCommand: function(sessionColl) {
        assert.commandWorked(sessionColl.deleteMany({a: 0, _id: {$gt: 0}}));
    },
    expectedChanges: [
        {
            documentKey: {a: 0, _id: 1},
            ns: {db: db.getName(), coll: collName},
            operationType: "delete",
        },
        {
            documentKey: {a: 0, _id: 2},
            ns: {db: db.getName(), coll: collName},
            operationType: "delete",
        },
        {
            documentKey: {a: 0, _id: 3},
            ns: {db: db.getName(), coll: collName},
            operationType: "delete",
        },
    ],
});

jsTestLog("Testing findAndModify() in a transaction.");
testDeleteInMultiDocTxn({
    collName: collName,
    deleteCommand: function(sessionColl) {
        sessionColl.findAndModify({query: {a: 0, _id: 1}, remove: true});
    },
    expectedChanges: [
        {
            documentKey: {a: 0, _id: 1},
            ns: {db: db.getName(), coll: collName},
            operationType: "delete",
        },
    ],
});

s.stop();
}());
