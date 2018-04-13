// Basic tests for $changeStream against all databases in the cluster.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest and
                                                       // assert[Valid|Invalid]ChangeStreamNss.

    db = db.getSiblingDB(jsTestName());
    const adminDB = db.getSiblingDB("admin");
    const otherDB = db.getSiblingDB(jsTestName() + "_other");

    // Drop and recreate the collections to be used in this set of tests.
    assertDropAndRecreateCollection(db, "t1");
    assertDropAndRecreateCollection(otherDB, "t2");

    // Test that a change stream can be opened on the admin database if {allChangesForCluster:true}
    // is specified.
    assertValidChangeStreamNss("admin", 1, {allChangesForCluster: true});
    // Test that a change stream cannot be opened on the admin database if a collection is
    // specified, even with {allChangesForCluster:true}.
    assertInvalidChangeStreamNss("admin", "testcoll", {allChangesForCluster: true});
    // Test that a change stream cannot be opened on a database other than admin if
    // {allChangesForCluster:true} is specified.
    assertInvalidChangeStreamNss(db.getName(), 1, {allChangesForCluster: true});

    let cst = new ChangeStreamTest(adminDB);
    let cursor = cst.startWatchingAllChangesForCluster();

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

    // Test that the change stream returns another inserted doc in a different database.
    assert.writeOK(otherDB.t2.insert({_id: 0, a: 2}));
    expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 2},
        ns: {db: otherDB.getName(), coll: "t2"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Dropping a database should generate drop entries for each collection followed by an
    // invalidate.
    // TODO SERVER-35029: This test should not invalidate the stream once there's support for
    // returning a notification for the dropDatabase command.
    assert.commandWorked(otherDB.dropDatabase());
    expected = [
        {operationType: "drop", ns: {db: otherDB.getName(), coll: "t2"}},
        {operationType: "invalidate"}
    ];

    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

    // Drop the remaining database and clean up the test.
    assert.commandWorked(db.dropDatabase());
    cst.cleanUp();
}());
