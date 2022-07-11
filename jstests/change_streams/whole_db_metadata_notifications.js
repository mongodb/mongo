// Tests of metadata notifications for a $changeStream on a whole database.
// Do not run in whole-cluster passthrough since this test assumes that the change stream will be
// invalidated by a database drop.
// @tags: [do_not_run_in_whole_cluster_passthrough]
(function() {
"use strict";

load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest
load('jstests/replsets/libs/two_phase_drops.js');  // For 'TwoPhaseDropCollectionTest'.
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.

const testDB = db.getSiblingDB(jsTestName());
testDB.dropDatabase();
let cst = new ChangeStreamTest(testDB);

// Write a document to the collection and test that the change stream returns it
// and getMore command closes the cursor afterwards.
const collName = "test";
let coll = assertDropAndRecreateCollection(testDB, collName);

let aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

// Create oplog entries of type insert, update, and delete.
assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(coll.update({_id: 1}, {$set: {a: 1}}));
assert.commandWorked(coll.remove({_id: 1}));

// Drop and recreate the collection.
const collAgg = assertDropAndRecreateCollection(testDB, collName);

// We should get 4 oplog entries of type insert, update, delete, and drop.
let change = cst.getOneChange(aggCursor);
assert.eq(change.operationType, "insert", tojson(change));
change = cst.getOneChange(aggCursor);
assert.eq(change.operationType, "update", tojson(change));
change = cst.getOneChange(aggCursor);
assert.eq(change.operationType, "delete", tojson(change));
change = cst.getOneChange(aggCursor);
assert.eq(change.operationType, "drop", tojson(change));

// Get a valid resume token that the next change stream can use.
assert.commandWorked(collAgg.insert({_id: 1}));

change = cst.getOneChange(aggCursor, false);
const resumeToken = change._id;

// For whole-db streams, it is possible to resume at a point before a collection is dropped.
assertDropCollection(testDB, collAgg.getName());
// Wait for two-phase drop to complete, so that the UUID no longer exists.
assert.soon(function() {
    return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(testDB, collAgg.getName());
});
assert.commandWorked(testDB.runCommand(
    {aggregate: 1, pipeline: [{$changeStream: {resumeAfter: resumeToken}}], cursor: {}}));

// Test that invalidation entries for other databases are filtered out.
const otherDB = testDB.getSiblingDB(jsTestName() + "other");
const otherDBColl = otherDB[collName + "_other"];
assert.commandWorked(otherDBColl.insert({_id: 0}));

// Create collection on the database being watched.
coll = assertDropAndRecreateCollection(testDB, collName);

// Create the $changeStream. We set 'doNotModifyInPassthroughs' so that this test will not be
// upconverted to a cluster-wide stream, which would return an entry for the dropped collection
// in the other database.
aggCursor = cst.startWatchingChanges(
    {pipeline: [{$changeStream: {}}], collection: 1, doNotModifyInPassthroughs: true});

// Drop the collection on the other database, this should *not* invalidate the change stream.
assertDropCollection(otherDB, otherDBColl.getName());

// Insert into the collection in the watched database, and verify the change stream is able to
// pick it up.
assert.commandWorked(coll.insert({_id: 1}));
change = cst.getOneChange(aggCursor);
assert.eq(change.operationType, "insert", tojson(change));
assert.eq(change.documentKey._id, 1);

// Test that renaming a collection generates a 'rename' entry for the 'from' collection. MongoDB
// does not allow renaming of sharded collections, so only perform this test if the collection
// is not sharded.
if (!FixtureHelpers.isSharded(coll)) {
    assertDropAndRecreateCollection(testDB, coll.getName());
    assertDropCollection(testDB, "renamed_coll");
    aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    assert.commandWorked(coll.renameCollection("renamed_coll"));
    cst.assertNextChangesEqual({
        cursor: aggCursor,
        expectedChanges: [{
            operationType: "rename",
            ns: {db: testDB.getName(), coll: coll.getName()},
            to: {db: testDB.getName(), coll: "renamed_coll"}
        }]
    });

    // Repeat the test, this time using the 'dropTarget' option with an existing target
    // collection.
    coll = testDB["renamed_coll"];
    assertCreateCollection(testDB, collName);
    assert.commandWorked(testDB[collName].insert({_id: 0}));
    assert.commandWorked(coll.renameCollection(collName, true /* dropTarget */));
    cst.assertNextChangesEqual({
        cursor: aggCursor,
        expectedChanges: [
            {
                operationType: "insert",
                ns: {db: testDB.getName(), coll: collName},
                documentKey: {_id: 0},
                fullDocument: {_id: 0}
            },
            {
                operationType: "rename",
                ns: {db: testDB.getName(), coll: "renamed_coll"},
                to: {db: testDB.getName(), coll: collName}
            }
        ]
    });

    coll = testDB[collName];
    // Test renaming a collection from the database being watched to a different database. Do
    // not run this in the mongos passthrough suites since we cannot guarantee the primary shard
    // of the target database, and renameCollection requires the source and destination to be on
    // the same shard.
    if (!FixtureHelpers.isMongos(testDB)) {
        const otherDB = testDB.getSiblingDB(testDB.getName() + "_rename_target");
        // Create target collection to ensure the database exists.
        const collOtherDB = assertCreateCollection(otherDB, "test");
        assertDropCollection(otherDB, "test");
        assert.commandWorked(testDB.adminCommand(
            {renameCollection: coll.getFullName(), to: collOtherDB.getFullName()}));
        // Rename across databases drops the source collection after the collection is copied
        // over.
        cst.assertNextChangesEqual({
            cursor: aggCursor,
            expectedChanges:
                [{operationType: "drop", ns: {db: testDB.getName(), coll: coll.getName()}}]
        });

        // Test renaming a collection from a different database to the database being watched.
        assert.commandWorked(testDB.adminCommand(
            {renameCollection: collOtherDB.getFullName(), to: coll.getFullName()}));
        // Do not check the 'ns' field since it will contain the namespace of the temp
        // collection created when renaming a collection across databases.
        change = cst.getOneChange(aggCursor);
        assert.eq(change.operationType, "rename");
        assert.eq(change.to, {db: testDB.getName(), coll: coll.getName()});
    }

    // Test the behavior of a change stream watching the target collection of a $out aggregation
    // stage.
    coll.aggregate([{$out: "renamed_coll"}]);
    // Note that $out will first create a temp collection, and then rename the temp collection
    // to the target. Do not explicitly check the 'ns' field.
    const rename = cst.getOneChange(aggCursor);
    assert.eq(rename.operationType, "rename", tojson(rename));
    assert.eq(rename.to, {db: testDB.getName(), coll: "renamed_coll"}, tojson(rename));

    // The change stream should not be invalidated by the rename(s).
    assert.eq(0, cst.getNextBatch(aggCursor).nextBatch.length);
    assert.commandWorked(coll.insert({_id: 2}));
    assert.eq(cst.getOneChange(aggCursor).operationType, "insert");

    // Drop the new collection to avoid an additional 'drop' notification when the database is
    // dropped.
    assertDropCollection(testDB, "renamed_coll");
    cst.assertNextChangesEqual({
        cursor: aggCursor,
        expectedChanges:
            [{operationType: "drop", ns: {db: testDB.getName(), coll: "renamed_coll"}}],
    });
}

// Dropping a collection should return a 'drop' entry.
assertDropCollection(testDB, coll.getName());
cst.assertNextChangesEqual({
    cursor: aggCursor,
    expectedChanges: [{operationType: "drop", ns: {db: testDB.getName(), coll: coll.getName()}}],
});

// Operations on internal "system" collections should be filtered out and not included in the
// change stream.
aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
// Creating a view will generate an insert entry on the "system.views" collection.
assert.commandWorked(testDB.runCommand({create: "view1", viewOn: coll.getName(), pipeline: []}));
// Drop the "system.views" collection.
assertDropCollection(testDB, "system.views");
// Verify that the change stream does not report the insertion into "system.views", and is
// not invalidated by dropping the system collection. Instead, it correctly reports the next
// write to the test collection.
assert.commandWorked(coll.insert({_id: 0}));
change = cst.getOneChange(aggCursor);
assert.eq(change.operationType, "insert", tojson(change));
assert.eq(change.ns, {db: testDB.getName(), coll: coll.getName()});

// Dropping the database should generate a 'dropDatabase' notification followed by an
// 'invalidate'.
assert.commandWorked(testDB.dropDatabase());
cst.assertDatabaseDrop({cursor: aggCursor, db: testDB});
const invalidateEvent = cst.assertNextChangesEqual(
    {cursor: aggCursor, expectedChanges: [{operationType: "invalidate"}]});

// Even after the 'invalidate' event has been filtered out, the cursor should hold the resume token
// of the 'invalidate' event.
const resumeStream =
    testDB.watch([{$match: {operationType: "DummyOperationType"}}], {resumeAfter: change._id});
assert.soon(() => {
    assert(!resumeStream.hasNext());
    return resumeStream.isExhausted();
});
assert.eq(resumeStream.getResumeToken(), invalidateEvent[0]._id);

cst.cleanUp();
}());
