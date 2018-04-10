// Tests of $changeStream invalidate entries.

(function() {
    "use strict";

    load('jstests/replsets/libs/two_phase_drops.js');  // For 'TwoPhaseDropCollectionTest'.
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    db.getMongo().forceReadMode('commands');

    // Write a document to the collection and test that the change stream returns it
    // and getMore command closes the cursor afterwards.
    const collGetMore = assertDropAndRecreateCollection(db, "change_stream_getmore_invalidations");
    // We awaited the replication of the first write, so the change stream shouldn't return it.
    assert.writeOK(collGetMore.insert({_id: 0, a: 1}));

    let changeStream = collGetMore.watch();

    // Drop the collection and test that we return "invalidate" entry and close the cursor. However,
    // we return all oplog entries preceding the drop.
    jsTestLog("Testing getMore command closes cursor for invalidate entries");
    // Create oplog entries of type insert, update, and delete.
    assert.writeOK(collGetMore.insert({_id: 1}));
    assert.writeOK(collGetMore.update({_id: 1}, {$set: {a: 1}}));
    assert.writeOK(collGetMore.remove({_id: 1}));
    // Drop the collection.
    assert.commandWorked(db.runCommand({drop: collGetMore.getName()}));

    // We should get 4 oplog entries of type insert, update, delete, and invalidate. The cursor
    // should be closed.
    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().operationType, "insert");
    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().operationType, "update");
    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().operationType, "delete");
    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().operationType, "invalidate");
    assert(changeStream.isExhausted());

    jsTestLog("Testing aggregate command closes cursor for invalidate entries");
    const collAgg = assertDropAndRecreateCollection(db, "change_stream_agg_invalidations");

    // Get a valid resume token that the next aggregate command can use.
    changeStream = collAgg.watch();

    assert.writeOK(collAgg.insert({_id: 1}));

    assert.soon(() => changeStream.hasNext());
    let change = changeStream.next();
    assert.eq(change.operationType, "insert");
    assert.eq(change.documentKey, {_id: 1});
    const resumeToken = change._id;

    // Insert another document after storing the resume token.
    assert.writeOK(collAgg.insert({_id: 2}));

    assert.soon(() => changeStream.hasNext());
    change = changeStream.next();
    assert.eq(change.operationType, "insert");
    assert.eq(change.documentKey, {_id: 2});

    // Drop the collection and invalidate the change stream.
    assertDropCollection(db, collAgg.getName());
    // Wait for two-phase drop to complete, so that the UUID no longer exists.
    assert.soon(function() {
        return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(db, collAgg.getName());
    });

    // Resume the change stream after the collection drop, up to and including the invalidate. This
    // is allowed if an explicit collation is provided.
    changeStream = collAgg.watch([], {resumeAfter: resumeToken, collation: {locale: "simple"}});

    assert.soon(() => changeStream.hasNext());
    change = changeStream.next();
    assert.eq(change.operationType, "insert");
    assert.eq(change.documentKey, {_id: 2});

    assert.soon(() => changeStream.hasNext());
    change = changeStream.next();
    assert.eq(change.operationType, "invalidate");
    assert(changeStream.isExhausted());

    // Test that it is possible to open a new change stream cursor on a collection that does not
    // exist.
    jsTestLog("Testing aggregate command on nonexistent collection");
    const collDoesNotExistName = "change_stream_agg_invalidations_does_not_exist";
    assertDropCollection(db, collDoesNotExistName);

    // Cursor creation succeeds, but there are no results.
    const cursorObj = assert
                          .commandWorked(db.runCommand({
                              aggregate: collDoesNotExistName,
                              pipeline: [{$changeStream: {}}],
                              cursor: {batchSize: 1},
                          }))
                          .cursor;

    // We explicitly test getMore, to ensure that the getMore command for a non-existent collection
    // does not return an error.
    let getMoreResult =
        assert
            .commandWorked(db.runCommand(
                {getMore: cursorObj.id, collection: collDoesNotExistName, batchSize: 1}))
            .cursor;
    assert.neq(getMoreResult.id, 0);
    assert.eq(getMoreResult.nextBatch.length, 0, tojson(getMoreResult.nextBatch));

    // After collection creation, we see oplog entries for the collection.
    const collNowExists = assertCreateCollection(db, collDoesNotExistName);
    assert.writeOK(collNowExists.insert({_id: 0}));

    assert.soon(function() {
        getMoreResult =
            assert
                .commandWorked(db.runCommand(
                    {getMore: cursorObj.id, collection: collDoesNotExistName, batchSize: 1}))
                .cursor;
        assert.neq(getMoreResult.id, 0);
        return getMoreResult.nextBatch.length > 0;
    }, "Timed out waiting for another result from getMore on non-existent collection.");
    assert.eq(getMoreResult.nextBatch.length, 1);
    assert.eq(getMoreResult.nextBatch[0].operationType, "insert");
    assert.eq(getMoreResult.nextBatch[0].documentKey, {_id: 0});

    assert.commandWorked(
        db.runCommand({killCursors: collDoesNotExistName, cursors: [getMoreResult.id]}));
}());
