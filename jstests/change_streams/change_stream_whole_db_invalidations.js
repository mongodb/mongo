// Tests of invalidate entries for a $changeStream on a whole database.
(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
    load('jstests/replsets/libs/two_phase_drops.js');  // For 'TwoPhaseDropCollectionTest'.
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.

    const testDB = db.getSiblingDB(jsTestName());
    let cst = new ChangeStreamTest(testDB);

    // Write a document to the collection and test that the change stream returns it
    // and getMore command closes the cursor afterwards.
    let coll = assertDropAndRecreateCollection(testDB, "change_stream_whole_db_invalidations");

    let aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    // Create oplog entries of type insert, update, and delete.
    assert.writeOK(coll.insert({_id: 1}));
    assert.writeOK(coll.update({_id: 1}, {$set: {a: 1}}));
    assert.writeOK(coll.remove({_id: 1}));
    // Drop the collection.
    assert.commandWorked(testDB.runCommand({drop: coll.getName()}));
    // We should get 4 oplog entries of type insert, update, delete, and invalidate. The cursor
    // should be closed.
    let change = cst.getOneChange(aggCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    change = cst.getOneChange(aggCursor);
    assert.eq(change.operationType, "update", tojson(change));
    change = cst.getOneChange(aggCursor);
    assert.eq(change.operationType, "delete", tojson(change));
    cst.assertNextChangesEqual({
        cursor: aggCursor,
        expectedChanges: [{operationType: "invalidate"}],
        expectInvalidate: true
    });

    const collAgg =
        assertDropAndRecreateCollection(testDB, "change_stream_whole_db_agg_invalidations");

    // Get a valid resume token that the next change stream can use.
    aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    assert.writeOK(collAgg.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    change = cst.getOneChange(aggCursor, false);
    const resumeToken = change._id;

    // For whole-db streams, it is possible to resume at a point before a collection is dropped,
    // even if the invalidation has not been received on the original stream yet.
    assertDropCollection(testDB, collAgg.getName());
    // Wait for two-phase drop to complete, so that the UUID no longer exists.
    assert.soon(function() {
        return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(testDB,
                                                                             collAgg.getName());
    });
    assert.commandWorked(testDB.runCommand(
        {aggregate: 1, pipeline: [{$changeStream: {resumeAfter: resumeToken}}], cursor: {}}));

    // Test that invalidation entries for other databases are filtered out.
    const otherDB = testDB.getSiblingDB("change_stream_whole_db_invalidations_other");
    const otherDBColl = otherDB["change_stream_whole_db_invalidations_other"];
    assert.writeOK(otherDBColl.insert({_id: 0}));

    // Create collection on the database being watched.
    coll = assertDropAndRecreateCollection(testDB, "change_stream_whole_db_invalidations");

    // Create the $changeStream. We set 'doNotModifyInPassthroughs' so that this test will not be
    // upconverted to a cluster-wide stream, which *would* be invalidated by dropping the collection
    // in the other database.
    aggCursor = cst.startWatchingChanges(
        {pipeline: [{$changeStream: {}}], collection: 1, doNotModifyInPassthroughs: true});

    // Drop the collection on the other database, this should *not* invalidate the change stream.
    assertDropCollection(otherDB, otherDBColl.getName());

    // Insert into the collection in the watched database, and verify the change stream is able to
    // pick it up.
    assert.writeOK(coll.insert({_id: 1}));
    change = cst.getOneChange(aggCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 1);

    // Test that renaming a collection will invalidate the change stream. MongoDB does not allow
    // renaming of sharded collections, so only perform this test if the collection is not sharded.
    if (!FixtureHelpers.isSharded(coll)) {
        assertDropCollection(testDB, coll.getName());

        assertCreateCollection(testDB, coll.getName());
        assertDropCollection(testDB, "renamed_coll");
        aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
        assert.writeOK(coll.renameCollection("renamed_coll"));
        cst.assertNextChangesEqual({
            cursor: aggCursor,
            expectedChanges: [{operationType: "invalidate"}],
            expectInvalidate: true
        });
    }

    // Dropping a collection should invalidate the change stream.
    assertDropCollection(testDB, coll.getName());
    aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    assertCreateCollection(testDB, coll.getName());
    assertDropCollection(testDB, coll.getName());
    cst.assertNextChangesEqual({
        cursor: aggCursor,
        expectedChanges: [{operationType: "invalidate"}],
        expectInvalidate: true
    });

    // Drop the database in order to clear the system and non-system test collections before running
    // the rename tests.
    assert.commandWorked(testDB.dropDatabase());

    // Operations on internal "system" collections should be filtered out and not included in the
    // change stream.
    aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    // Creating a view will generate an insert entry on the "system.views" collection.
    assert.commandWorked(
        testDB.runCommand({create: "view1", viewOn: coll.getName(), pipeline: []}));
    // Drop the "system.views" collection.
    assertDropCollection(testDB, "system.views");
    // Verify that the change stream does not report the insertion into "system.views", and is
    // not invalidated by dropping the system collection. Instead, it correctly reports the next
    // write to the test collection.
    assert.writeOK(coll.insert({_id: 0}));
    change = cst.getOneChange(aggCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.ns, {db: testDB.getName(), coll: coll.getName()});

    // Test that renaming a "system" collection *does* invalidate the stream if the target of
    // the rename is a non-system collection.
    assert.commandWorked(
        testDB.runCommand({create: "view1", viewOn: coll.getName(), pipeline: []}));
    assert.writeOK(testDB.system.views.renameCollection("non_system_collection"));
    cst.assertNextChangesEqual({
        cursor: aggCursor,
        expectedChanges: [{operationType: "invalidate"}],
        expectInvalidate: true
    });

    // Test that renaming a "system" collection to a different "system" collection does not
    // result in a notification in the change stream.
    aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    assert.commandWorked(
        testDB.runCommand({create: "view1", viewOn: coll.getName(), pipeline: []}));
    // Note that the target of the rename must be a valid "system" collection.
    assert.writeOK(testDB.system.views.renameCollection("system.js"));
    // Verify that the change stream filters out the rename, instead returning the next insert to
    // the test collection.
    assert.writeOK(coll.insert({_id: 1}));
    change = cst.getOneChange(aggCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.ns, {db: testDB.getName(), coll: coll.getName()});

    // Test that renaming a user collection to a "system" collection *does* invalidate the stream.
    assert.writeOK(coll.renameCollection("system.views"));
    cst.assertNextChangesEqual({
        cursor: aggCursor,
        expectedChanges: [{operationType: "invalidate"}],
        expectInvalidate: true
    });

    // Drop the "system.views" collection to avoid view catalog errors in subsequent tests.
    assertDropCollection(testDB, "system.views");

    cst.cleanUp();
}());
