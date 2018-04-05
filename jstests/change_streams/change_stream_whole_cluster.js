// Basic tests for $changeStream against all databases in the cluster.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest and
                                                       // assert[Valid|Invalid]ChangeStreamNss.

    const adminDB = db.getSiblingDB("admin");
    const otherDB = db.getSiblingDB(`${db.getName()}_other`);

    let cst = new ChangeStreamTest(adminDB);
    let cursor = cst.startWatchingAllChangesForCluster();

    assertCreateCollection(db, "t1");
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

    // Dropping either database should invalidate the change stream.
    assert.commandWorked(otherDB.dropDatabase());
    expected = {operationType: "invalidate"};
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Drop the remaining database and clean up the test.
    assert.commandWorked(db.dropDatabase());
    cst.cleanUp();
}());
