// Tests of $changeStream invalidate entries.

(function() {
    "use strict";

    load('jstests/libs/uuid_util.js');

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

    // Drop the collection and test that we return "invalidate" entry and close the cursor.
    jsTestLog("Testing getMore command closes cursor for invalidate entries");
    collGetMore.drop();
    let res = assert.commandWorked(
        db.runCommand({getMore: aggcursor.id, collection: collGetMore.getName()}));
    aggcursor = res.cursor;
    assert.eq(aggcursor.id, 0, "expected invalidation to cause the cursor to be closed");
    assert.eq(aggcursor.nextBatch.length, 1);
    assert.docEq(aggcursor.nextBatch[0],
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
    // Wait for the drop to actually happen.
    assert.soon(function() {
        const visibleRes = assert.commandWorked(db.runCommand("listCollections"));
        const allRes =
            assert.commandWorked(db.runCommand("listCollections", {includePendingDrops: true}));
        const visibleCollections = visibleRes.cursor.firstBatch;
        const allCollections = allRes.cursor.firstBatch;
        return visibleCollections.length == allCollections.length;
    });
    res = assert.commandFailed(db.runCommand({
        aggregate: collAgg.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeToken}}, oplogProjection],
        cursor: {}
    }));
}());
