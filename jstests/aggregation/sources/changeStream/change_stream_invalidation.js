// Tests of $changeStream invalidate entries.

(function() {
    "use strict";

    // Strip the oplog fields we aren't testing.
    const oplogProjection = {$project: {"_id.ts": 0}};

    // Helpers for testing that pipeline returns correct set of results.  Run startWatchingChanges
    // with the pipeline, then insert the changes, then run assertNextBatchMatches with the result
    // of startWatchingChanges and the expected set of results.
    function startWatchingChanges(pipeline, collection) {
        let res = assert.commandWorked(
            db.runCommand({aggregate: collection.getName(), "pipeline": pipeline, cursor: {}}));
        assert.neq(res.cursor.id, 0);
        return res.cursor;
    }

    const replTest = new ReplSetTest({name: 'changeStreamTest', nodes: 1});
    const nodes = replTest.startSet();
    replTest.initiate();
    replTest.awaitReplication();

    db = replTest.getPrimary().getDB('test');
    db.getMongo().forceReadMode('commands');

    // Write a document to the collection and test that the change stream returns it
    // and getMore command closes the cursor afterwards.
    const collGetMore = db.change_stream_getmore_invalidations;
    // We awaited the replicaiton of the first write, so the change stream shouldn't return it.
    // Use { w: "majority" } to deal with journaling correctly, even though we only have one node.
    assert.writeOK(collGetMore.insert({_id: 0, a: 1}, {writeConcern: {w: "majority"}}));
    let aggcursor = startWatchingChanges([{$changeStream: {}}, oplogProjection], collGetMore);
    assert.neq(aggcursor.id, 0);
    assert.eq(aggcursor.firstBatch.length, 0);

    // Drop the collection and test that we return "invalidate" entry and close the cursor.
    jsTestLog("Testing getMore command closes cursor for invalidate entries");
    collGetMore.drop();
    let res = assert.commandWorked(
        db.runCommand({getMore: aggcursor.id, collection: collGetMore.getName()}));
    aggcursor = res.cursor;
    assert.eq(aggcursor.id, 0, "expected invalidation to cause the cursor to be closed");
    assert.eq(aggcursor.nextBatch.length, 1);
    assert.docEq(aggcursor.nextBatch[0],
                 {_id: {ns: "test.$cmd"}, fullDocument: null, operationType: "invalidate"});

    jsTestLog("Testing aggregate command closes cursor for invalidate entries");
    const collAgg = db.change_stream_agg_invalidations;
    db.createCollection(collAgg.getName());
    // Get a valid resume token that the next aggregate command can use.
    aggcursor = startWatchingChanges([{$changeStream: {}}], collAgg);
    assert.eq(aggcursor.firstBatch.length, 0);

    assert.writeOK(collAgg.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    res =
        assert.commandWorked(db.runCommand({getMore: aggcursor.id, collection: collAgg.getName()}));
    aggcursor = res.cursor;
    assert.neq(aggcursor.id, 0);
    assert.eq(aggcursor.nextBatch.length, 1);
    const resumeToken = aggcursor.nextBatch[0]._id;

    assert(collAgg.drop());
    res = assert.commandWorked(db.runCommand({
        aggregate: collAgg.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeToken}}, oplogProjection],
        cursor: {}
    }));
    aggcursor = res.cursor;
    assert.eq(aggcursor.id, 0, "expected invalidation to cause the cursor to be closed");
    assert.eq(aggcursor.firstBatch.length, 1);
    assert.docEq(aggcursor.firstBatch,
                 [{_id: {ns: "test.$cmd"}, fullDocument: null, operationType: "invalidate"}]);

    replTest.stopSet();
}());
