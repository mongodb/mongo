// Tests of $changeStream invalidate entries.

(function() {
    "use strict";

    load('jstests/libs/uuid_util.js');
    load('jstests/replsets/libs/two_phase_drops.js');  // For 'TwoPhaseDropCollectionTest'.

    // Strip the oplog fields we aren't testing.
    const oplogProjection = {$project: {"_id.clusterTime": 0}};

    // Helpers for testing that pipeline returns correct set of results.  Run startWatchingChanges
    // with the pipeline, then insert the changes, then run assertNextBatchMatches with the result
    // of startWatchingChanges and the expected set of results.
    function startWatchingChanges(pipeline, collection) {
        // TODO: SERVER-29126
        // While change streams still uses read concern level local instead of read concern level
        // majority, we need to use causal consistency to be able to immediately read our own writes
        // out of the oplog.  Once change streams read from the majority snapshot, we can remove
        // these synchronization points from this test.
        assert.commandWorked(db.runCommand({
            find: "foo",
            readConcern: {level: "local", afterClusterTime: db.getSession().getOperationTime()}
        }));

        let res = assert.commandWorked(
            db.runCommand({aggregate: collection.getName(), "pipeline": pipeline, cursor: {}}));
        assert.neq(res.cursor.id, 0);
        return res.cursor;
    }

    db.getMongo().forceReadMode('commands');

    // Write a document to the collection and test that the change stream returns it
    // and getMore command closes the cursor afterwards.
    const collGetMore = db.change_stream_getmore_invalidations;
    // We awaited the replication of the first write, so the change stream shouldn't return it.
    // Use { w: "majority" } to deal with journaling correctly, even though we only have one node.
    assert.writeOK(collGetMore.insert({_id: 0, a: 1}, {writeConcern: {w: "majority"}}));

    let aggcursor = startWatchingChanges([{$changeStream: {}}, oplogProjection], collGetMore);
    assert.neq(aggcursor.id, 0);
    assert.eq(aggcursor.firstBatch.length, 0);

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
    let res = assert.commandWorked(
        db.runCommand({getMore: aggcursor.id, collection: collGetMore.getName()}));
    aggcursor = res.cursor;
    assert.eq(aggcursor.id, 0, "expected invalidation to cause the cursor to be closed");
    assert.eq(aggcursor.nextBatch.length, 4, tojson(aggcursor.nextBatch));
    assert.eq(aggcursor.nextBatch[0].operationType, "insert", tojson(aggcursor.nextBatch));
    assert.eq(aggcursor.nextBatch[1].operationType, "update", tojson(aggcursor.nextBatch));
    assert.eq(aggcursor.nextBatch[2].operationType, "delete", tojson(aggcursor.nextBatch));
    assert.docEq(aggcursor.nextBatch[3],
                 {_id: {uuid: collGetMoreUuid}, operationType: "invalidate"});

    jsTestLog("Testing aggregate command closes cursor for invalidate entries");
    const collAgg = db.change_stream_agg_invalidations;
    db.createCollection(collAgg.getName());
    const collAggUuid = getUUIDFromListCollections(db, collAgg.getName());
    // Get a valid resume token that the next aggregate command can use.
    aggcursor = startWatchingChanges([{$changeStream: {}}], collAgg);
    assert.eq(aggcursor.firstBatch.length, 0);

    assert.writeOK(collAgg.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    // TODO: SERVER-29126
    // While change streams still uses read concern level local instead of read concern level
    // majority, we need to use causal consistency to be able to immediately read our own writes
    // out of the oplog.  Once change streams read from the majority snapshot, we can remove
    // these synchronization points from this test.
    assert.commandWorked(db.runCommand({
        find: "foo",
        readConcern: {level: "local", afterClusterTime: db.getSession().getOperationTime()}
    }));

    res =
        assert.commandWorked(db.runCommand({getMore: aggcursor.id, collection: collAgg.getName()}));
    aggcursor = res.cursor;
    assert.neq(aggcursor.id, 0);
    assert.eq(aggcursor.nextBatch.length, 1);
    const resumeToken = aggcursor.nextBatch[0]._id;

    // It should not possible to resume a change stream after a collection drop, even if the
    // invalidate has not been received.
    assert(collAgg.drop());
    // Wait for two-phase drop to complete, so that the UUID no longer exists.
    assert.soon(function() {
        return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(db, collAgg.getName());
    });
    assert.commandFailed(db.runCommand({
        aggregate: collAgg.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeToken}}, oplogProjection],
        cursor: {}
    }));

    // Test that it is possible to open a new change stream cursor on a collection that does not
    // exist.
    jsTestLog("Testing aggregate command on nonexistent collection");
    const collDoesNotExist = db.change_stream_agg_invalidations_does_not_exist;
    db.runCommand({drop: collDoesNotExist.getName(), writeConcern: {j: true}});
    // Cursor creation succeeds, but there are no results.
    res = assert.commandWorked(db.runCommand({
        aggregate: collDoesNotExist.getName(),
        pipeline: [{$changeStream: {}}, oplogProjection],
        cursor: {}
    }));
    aggcursor = res.cursor;
    assert.neq(aggcursor.id, 0);
    // We explicitly test getMore, to ensure that the getMore command for a non-existent collection
    // does not return an error.
    res = assert.commandWorked(
        db.runCommand({getMore: res.cursor.id, collection: collDoesNotExist.getName()}));
    aggcursor = res.cursor;
    assert.neq(aggcursor.id, 0);
    assert.eq(aggcursor.nextBatch.length, 0, tojson(aggcursor.nextBatch));
    // After collection creation, we see oplog entries for the collection.
    assert.writeOK(collDoesNotExist.insert({_id: 0}, {writeConcern: {j: true}}));
    res = assert.commandWorked(
        db.runCommand({getMore: res.cursor.id, collection: collDoesNotExist.getName()}));
    aggcursor = res.cursor;
    assert.eq(aggcursor.nextBatch.length, 1, tojson(aggcursor.nextBatch));
    assert.eq(aggcursor.nextBatch[0].operationType, "insert", tojson(aggcursor.nextBatch));
}());
