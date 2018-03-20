// Tests resuming change streams based on cluster time.
(function() {
    "use strict";

    const coll = db[jsTestName()];
    coll.drop();

    assert.commandWorked(db.createCollection(coll.getName()));

    const testStartTime = db.runCommand({isMaster: 1}).$clusterTime.clusterTime;

    // Write a document to each chunk, and wait for replication.
    assert.writeOK(coll.insert({_id: -1}, {writeConcern: {w: "majority"}}));
    assert.writeOK(coll.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    // Perform two updates, capturing the cluster time of the first to be resumed from.
    const res = coll.runCommand("update", {updates: [{q: {_id: -1}, u: {$set: {updated: true}}}]});
    const resumeTime = res.$clusterTime.clusterTime;

    assert.writeOK(coll.update({_id: 1}, {$set: {updated: true}}));

    let changeStream = coll.watch([], {startAtClusterTime: {ts: resumeTime}});

    // Test that we see the two updates.
    assert.soon(() => changeStream.hasNext());
    let next = changeStream.next();
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
        pipeline: [{$changeStream: {startAtClusterTime: {ts: resumeTime}, resumeAfter: next._id}}],
        cursor: {}
    }),
                                 40674);

    assert.commandFailedWithCode(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{
            $changeStream: {
                startAtClusterTime: {ts: resumeTime},
                $_resumeAfterClusterTime: {ts: resumeTime}
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
