// Tests of $changeStream invalidate entries.

(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");
    load('jstests/libs/uuid_util.js');
    load('jstests/replsets/libs/two_phase_drops.js');  // For 'TwoPhaseDropCollectionTest'.

    let cst = new ChangeStreamTest(db);

    db.getMongo().forceReadMode('commands');

    // Write a document to the collection and test that the change stream returns it
    // and getMore command closes the cursor afterwards.
    const collGetMore = db.change_stream_getmore_invalidations;
    // We awaited the replication of the first write, so the change stream shouldn't return it.
    // Use { w: "majority" } to deal with journaling correctly, even though we only have one node.
    assert.writeOK(collGetMore.insert({_id: 0, a: 1}, {writeConcern: {w: "majority"}}));

    let aggcursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: collGetMore});

    const collGetMoreUuid = getUUIDFromListCollections(db, collGetMore.getName());

    // Drop the collection and test that we return "invalidate" entry and close the cursor. However,
    // we return all oplog entries preceding the drop.
    jsTestLog("Testing getMore command closes cursor for invalidate entries");
    // Create oplog entries of type insert, update, and delete.
    assert.writeOK(collGetMore.insert({_id: 1}));
    assert.writeOK(collGetMore.update({_id: 1}, {$set: {a: 1}}));
    assert.writeOK(collGetMore.remove({_id: 1}));
    // Drop the collection.
    assert.commandWorked(db.runCommand({drop: collGetMore.getName(), writeConcern: {j: true}}));
    // We should get 4 oplog entries of type insert, update, delete, and invalidate. The cursor
    // should be closed.
    let change = cst.getOneChange(aggcursor);
    assert.eq(change.operationType, "insert", tojson(change));
    change = cst.getOneChange(aggcursor);
    assert.eq(change.operationType, "update", tojson(change));
    change = cst.getOneChange(aggcursor);
    assert.eq(change.operationType, "delete", tojson(change));
    cst.assertNextChangesEqual({
        cursor: aggcursor,
        expectedChanges: [{operationType: "invalidate"}],
        expectInvalidate: true
    });

    jsTestLog("Testing aggregate command closes cursor for invalidate entries");
    const collAgg = db.change_stream_agg_invalidations;
    db.createCollection(collAgg.getName());
    const collAggUuid = getUUIDFromListCollections(db, collAgg.getName());
    // Get a valid resume token that the next aggregate command can use.
    aggcursor = cst.startWatchingChanges(
        {pipeline: [{$changeStream: {}}], collection: collAgg, includeToken: true});

    assert.writeOK(collAgg.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    change = cst.getOneChange(aggcursor, false);
    const resumeToken = change._id;

    // It should not possible to resume a change stream after a collection drop, even if the
    // invalidate has not been received.
    assert(collAgg.drop());
    // Wait for two-phase drop to complete, so that the UUID no longer exists.
    assert.soon(function() {
        return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(db, collAgg.getName());
    });
    assert.commandFailed(db.runCommand({
        aggregate: collAgg.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeToken}}, cst.oplogProjection],
        cursor: {}
    }));

    // Test that it is possible to open a new change stream cursor on a collection that does not
    // exist.
    jsTestLog("Testing aggregate command on nonexistent collection");
    const collDoesNotExist = db.change_stream_agg_invalidations_does_not_exist;
    db.runCommand({drop: collDoesNotExist.getName(), writeConcern: {j: true}});

    // Cursor creation succeeds, but there are no results.
    aggcursor = cst.startWatchingChanges({
        collection: collDoesNotExist,
        pipeline: [{$changeStream: {}}],
    });

    // We explicitly test getMore, to ensure that the getMore command for a non-existent collection
    // does not return an error.
    aggcursor = cst.getNextBatch(aggcursor);
    assert.neq(aggcursor.id, 0);
    assert.eq(aggcursor.nextBatch.length, 0, tojson(aggcursor.nextBatch));

    // After collection creation, we see oplog entries for the collection.
    assert.writeOK(collDoesNotExist.insert({_id: 0}, {writeConcern: {j: true}}));
    change = cst.getOneChange(aggcursor);
    assert.eq(change.operationType, "insert", tojson(change));

    cst.cleanUp();
}());
