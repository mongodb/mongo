// Tests of invalidate entries for a $changeStream on a whole database.
(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
    load('jstests/replsets/libs/two_phase_drops.js');  // For 'TwoPhaseDropCollectionTest'.
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    let cst = new ChangeStreamTest(db);

    // Write a document to the collection and test that the change stream returns it
    // and getMore command closes the cursor afterwards.
    let coll = assertDropAndRecreateCollection(db, "change_stream_whole_db_invalidations");

    let aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    // Create oplog entries of type insert, update, and delete.
    assert.writeOK(coll.insert({_id: 1}));
    assert.writeOK(coll.update({_id: 1}, {$set: {a: 1}}));
    assert.writeOK(coll.remove({_id: 1}));
    // Drop the collection.
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
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

    const collAgg = assertDropAndRecreateCollection(db, "change_stream_whole_db_agg_invalidations");

    // Get a valid resume token that the next change stream can use.
    aggCursor = cst.startWatchingChanges(
        {pipeline: [{$changeStream: {}}], collection: 1, includeToken: true});

    assert.writeOK(collAgg.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    change = cst.getOneChange(aggCursor, false);
    const resumeToken = change._id;

    // It should not possible to resume a change stream after a collection drop, even if the
    // invalidate has not been received.
    assertDropCollection(db, collAgg.getName());
    // Wait for two-phase drop to complete, so that the UUID no longer exists.
    assert.soon(function() {
        return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(db, collAgg.getName());
    });
    assert.commandFailedWithCode(db.runCommand({
        aggregate: 1,
        pipeline: [{$changeStream: {resumeAfter: resumeToken}}, cst.oplogProjection],
        cursor: {}
    }),
                                 40615);

    // Test that invalidation entries for other databases are filtered out.
    const otherDB = db.getSiblingDB("change_stream_whole_db_invalidations_other");
    const otherDBColl = otherDB["change_stream_whole_db_invalidations_other"];
    assert.writeOK(otherDBColl.insert({_id: 0}));

    // Create collection on the database being watched.
    coll = assertDropAndRecreateCollection(db, "change_stream_whole_db_invalidations");

    aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    // Drop the collection on the other database, this should *not* invalidate the change stream.
    assertDropCollection(otherDB, otherDBColl.getName());

    // Insert into the collection in the watched database, and verify the change stream is able to
    // pick it up.
    assert.writeOK(coll.insert({_id: 1}));
    change = cst.getOneChange(aggCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 1);

    // Dropping a collection should invalidate the change stream.
    assertDropCollection(db, coll.getName());
    cst.assertNextChangesEqual({
        cursor: aggCursor,
        expectedChanges: [{operationType: "invalidate"}],
        expectInvalidate: true
    });

    // Renaming a collection should invalidate the change stream.
    assertCreateCollection(db, coll.getName());
    assertDropCollection(db, "renamed_coll");
    aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    assert.writeOK(coll.renameCollection("renamed_coll"));
    cst.assertNextChangesEqual({
        cursor: aggCursor,
        expectedChanges: [{operationType: "invalidate"}],
        expectInvalidate: true
    });

    // Dropping a 'system' collection should not invalidate the change stream.
    aggCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    assertDropCollection(db, "system.views");

    assert.writeOK(coll.insert({_id: 2}));
    change = cst.getOneChange(aggCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 2);

    cst.cleanUp();
}());
