// Tests resuming change streams based on cluster time.
(function() {
    "use strict";
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    const coll = assertDropAndRecreateCollection(db, jsTestName());

    const testStartTime = db.runCommand({isMaster: 1}).$clusterTime.clusterTime;

    // Write a document to each chunk, and wait for replication.
    assert.writeOK(coll.insert({_id: -1}, {writeConcern: {w: "majority"}}));
    assert.writeOK(coll.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    // Perform two updates, then use a change stream to capture the cluster time of the first update
    // to be resumed from.
    const streamToFindClusterTime = coll.watch();
    assert.writeOK(coll.update({_id: -1}, {$set: {updated: true}}));
    assert.writeOK(coll.update({_id: 1}, {$set: {updated: true}}));
    assert.soon(() => streamToFindClusterTime.hasNext());
    let next = streamToFindClusterTime.next();
    assert.eq(next.operationType, "update");
    assert.eq(next.documentKey, {_id: -1});
    const clusterTimeOfFirstUpdate = next.clusterTime;

    let changeStream = coll.watch([], {startAtClusterTime: {ts: clusterTimeOfFirstUpdate}});

    // Test that starting at the cluster time is inclusive of the first update, so we should see
    // both updates in the new stream.
    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "update", tojson(next));
    assert.eq(next.documentKey._id, -1, tojson(next));

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "update", tojson(next));
    assert.eq(next.documentKey._id, 1, tojson(next));

    // Test that startAtClusterTime is not allowed alongside resumeAfter or
    // $_resumeAfterClusterTime.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{
            $changeStream:
                {startAtClusterTime: {ts: clusterTimeOfFirstUpdate}, resumeAfter: next._id}
        }],
        cursor: {}
    }),
                                 40674);

    assert.commandFailedWithCode(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{
            $changeStream: {
                startAtClusterTime: {ts: clusterTimeOfFirstUpdate},
                $_resumeAfterClusterTime: {ts: clusterTimeOfFirstUpdate}
            }
        }],
        cursor: {}
    }),
                                 50573);

    // Test that resuming from a time in the future will wait for that time to come.
    let resumeTimeFarFuture = db.runCommand({isMaster: 1}).$clusterTime.clusterTime;
    resumeTimeFarFuture =
        new Timestamp(resumeTimeFarFuture.getTime() + 60 * 60 * 6, 1);  // 6 hours in the future

    let changeStreamFuture = coll.watch([], {startAtClusterTime: {ts: resumeTimeFarFuture}});

    // Resume the change stream from the start of the test and verify it picks up the changes to the
    // collection. Namely, it should see two inserts followed by two updates.
    changeStream = coll.watch([], {startAtClusterTime: {ts: testStartTime}});
    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "insert", tojson(next));
    assert.eq(next.documentKey._id, -1, tojson(next));

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "insert", tojson(next));
    assert.eq(next.documentKey._id, 1, tojson(next));

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "update", tojson(next));
    assert.eq(next.documentKey._id, -1, tojson(next));

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "update", tojson(next));
    assert.eq(next.documentKey._id, 1, tojson(next));

    // Verify that the change stream resumed from far into the future does not see any changes.
    assert(!changeStreamFuture.hasNext());
})();
