// Basic tests for $changeStream against all collections in a database.
// Do not run in whole-cluster passthrough since this test assumes that the change stream will be
// invalidated by a database drop.
// @tags: [do_not_run_in_whole_cluster_passthrough]
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest and
                                                   // assert[Valid|Invalid]ChangeStreamNss.
load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.

db = db.getSiblingDB(jsTestName());
assert.commandWorked(db.dropDatabase());

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

// Test that the change stream returns an inserted doc on a user-created collection whose name
// includes "system" but is not considered an internal collection.
const validSystemColls = ["system", "systems.views", "ssystem.views", "test.system"];
validSystemColls.forEach(collName => {
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    const coll = db.getCollection(collName);
    assert.writeOK(coll.insert({_id: 0, a: 1}));
    expected = [
        {
            documentKey: {_id: 0},
            fullDocument: {_id: 0, a: 1},
            ns: {db: db.getName(), coll: collName},
            operationType: "insert",
        },
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

    // Drop the collection and verify that the change stream picks up the "drop" notification.
    assertDropCollection(db, collName);
    // Insert to the test collection to queue up another change after the drop. This is needed
    // since the number of 'drop' notifications is not deterministic in the sharded passthrough
    // suites.
    assert.writeOK(coll.insert({_id: 0}));
    cst.consumeDropUpTo({
        cursor: cursor,
        dropType: "drop",
        expectedNext: {
            documentKey: {_id: 0},
            fullDocument: {_id: 0},
            ns: {db: db.getName(), coll: collName},
            operationType: "insert",
        },
    });
});

cst.cleanUp();
}());
