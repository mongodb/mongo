// Basic tests for $changeStream against all collections in a database.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.

    // Test that a change stream cannot be opened on the "admin", "config", or "local" databases.
    // TODO SERVER-34040 Should prevent change streams on these databases.
    assert.commandWorked(db.getSiblingDB("admin").runCommand(
        {aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {}}));

    assert.commandWorked(db.getSiblingDB("config").runCommand(
        {aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {}}));

    assert.commandWorked(db.getSiblingDB("local").runCommand(
        {aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {}}));

    // Test that a change stream can be opened before a database exists.
    assert.commandWorked(db.dropDatabase());

    let cst = new ChangeStreamTest(db);
    let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    assertCreateCollection(db, "t1");
    // Test that if there are no changes, we return an empty batch.
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Test that the change stream returns an inserted doc.
    assert.writeOK(db.t1.insert({_id: 0, a: 1}));
    let expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 1},
        ns: {db: "test", coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Test that the change stream returns another inserted doc in a different collection but still
    // in the target db.
    assert.writeOK(db.t2.insert({_id: 0, a: 2}));
    expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 2},
        ns: {db: "test", coll: "t2"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Test that 'updateLookup' is not allowed with a change stream on an entire database.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: 1,
        pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
        cursor: {}
    }),
                                 50761);

    // Dropping the database should invalidate the change stream.
    assert.commandWorked(db.dropDatabase());
    expected = {operationType: "invalidate"};
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    cst.cleanUp();
}());
