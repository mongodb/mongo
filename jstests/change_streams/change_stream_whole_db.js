// Basic tests for $changeStream against all collections in a database.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest and
                                                       // assert[Valid|Invalid]ChangeStreamNss.
    load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.

    // Test that a change stream cannot be opened on the "admin", "config", or "local" databases.
    // TODO SERVER-34086: $changeStream may run against 'admin' if 'allChangesForCluster' is true.
    assertInvalidChangeStreamNss("admin", 1);
    assertInvalidChangeStreamNss("config", 1);
    if (!FixtureHelpers.isMongos(db)) {
        assertInvalidChangeStreamNss("local", 1);
    }

    assertDropCollection(db, "t1");
    assertDropCollection(db, "t2");

    assertCreateCollection(db, "t1");
    assertCreateCollection(db, "t2");

    let cst = new ChangeStreamTest(db);
    let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

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

    // Dropping the database should invalidate the change stream.
    assert.commandWorked(db.dropDatabase());
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [{operationType: "invalidate"}]});

    cst.cleanUp();
}());
