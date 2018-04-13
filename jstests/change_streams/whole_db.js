// Basic tests for $changeStream against all collections in a database.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest and
                                                       // assert[Valid|Invalid]ChangeStreamNss.
    load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.

    db = db.getSiblingDB(jsTestName());

    // Test that a single-database change stream cannot be opened on "admin", "config", or "local".
    assertInvalidChangeStreamNss("admin", 1);
    assertInvalidChangeStreamNss("config", 1);
    if (!FixtureHelpers.isMongos(db)) {
        assertInvalidChangeStreamNss("local", 1);
    }

    let cst = new ChangeStreamTest(db);
    let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    // Test that if there are no changes, we return an empty batch.
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Test that the change stream returns an inserted doc.
    assert.writeOK(db.t1.insert({_id: 0, a: 1}));
    let expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 1},
        ns: {db: db.getName(), coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Test that the change stream returns another inserted doc in a different collection but still
    // in the target db.
    assert.writeOK(db.t2.insert({_id: 0, a: 2}));
    expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 2},
        ns: {db: db.getName(), coll: "t2"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Dropping the database should generate collection drop entries followed by an invalidate. Note
    // that the order of collection drops is not guaranteed so only check the database name.
    assert.commandWorked(db.dropDatabase());
    let change = cst.getOneChange(cursor);
    assert.eq(change.operationType, "drop", tojson(change));
    assert.eq(change.ns.db, db.getName(), tojson(change));
    change = cst.getOneChange(cursor);
    assert.eq(change.operationType, "drop", tojson(change));
    assert.eq(change.ns.db, db.getName(), tojson(change));

    // TODO SERVER-35029: Expect to see a 'dropDatabase' entry before the invalidate.
    change = cst.getOneChange(cursor, true);
    assert.eq(change.operationType, "invalidate", tojson(change));

    cst.cleanUp();
}());
