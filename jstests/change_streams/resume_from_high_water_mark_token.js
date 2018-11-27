/**
 * Tests that a synthetic high-water-mark (HWM) token obeys the same semantics as a regular token.
 */
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");  // For runCommandChangeStreamPassthroughAware.

    // Drop and recreate collections to assure a clean run.
    const collName = jsTestName();
    const testCollection = assertDropAndRecreateCollection(db, collName);
    const otherCollection = assertDropAndRecreateCollection(db, "unrelated_" + collName);
    const adminDB = db.getSiblingDB("admin");

    let docId = 0;  // Tracks _id of documents inserted to ensure that we do not duplicate.

    // Open a stream on the test collection. Write one document to the test collection and one to
    // the unrelated collection, in order to push the postBatchResumeToken (PBRT) past the last
    // related event.
    let csCursor = testCollection.watch();
    assert.commandWorked(testCollection.insert({_id: docId++}));
    assert.commandWorked(otherCollection.insert({}));

    // Consume all events. The PBRT of the batch should be greater than the last event, which
    // guarantees that it is a synthetic high-water-mark token.
    let relatedEvent = null;
    let hwmToken = null;
    assert.soon(() => {
        if (csCursor.hasNext()) {
            relatedEvent = csCursor.next();
        }
        assert.eq(csCursor.objsLeftInBatch(), 0);
        hwmToken = csCursor.getResumeToken();
        assert.neq(undefined, hwmToken);
        return relatedEvent && bsonWoCompare(hwmToken, relatedEvent._id) > 0;
    });

    // Now write some further documents to the collection before attempting to resume.
    for (let i = 0; i < 5; ++i) {
        assert.commandWorked(testCollection.insert({_id: docId++}));
    }

    // Resume the stream from the high water mark. We only see the latest 5 documents.
    csCursor = testCollection.watch([], {resumeAfter: hwmToken});
    assert.soon(() => {
        if (csCursor.hasNext()) {
            relatedEvent = csCursor.next();
            assert.gt(bsonWoCompare(relatedEvent._id, hwmToken), 0);
            // We never see the first document, whose _id was 0.
            assert.gt(relatedEvent.fullDocument._id, 0);
        }
        // The _id of the last document inserted is (docId-1).
        return relatedEvent.fullDocument._id === (docId - 1);
    });

    // Confirm that we cannot use a high-water-mark token with startAtOperationTime.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: testCollection.getName(),
        pipeline: [{$changeStream: {startAtOperationTime: hwmToken}}],
        cursor: {}
    }),
                                 ErrorCodes.TypeMismatch);

    // Confirm that a high water mark can resume a stream on a collection with a default collation.
    const collationOpts = {collation: {locale: "en_US", strength: 2}};
    const collationCollName = "collation_" + collName;
    const testCollationCollection =
        assertDropAndRecreateCollection(db, collationCollName, collationOpts);

    // Opening a change stream with batchSize:0 is guaranteed to produce a high water mark.
    csCursor = testCollationCollection.watch([], {cursor: {batchSize: 0}});
    hwmToken = csCursor.getResumeToken();
    assert.neq(undefined, hwmToken);

    assert.commandWorked(db.runCommand({
        aggregate: collationCollName,
        pipeline: [{$changeStream: {resumeAfter: hwmToken}}],
        cursor: {}
    }));

    // Confirm that a high water mark cannot be used to resume a stream on a collection that has
    // been dropped, unless the client specifies an explicit collation. Be sure to exempt this
    // command from modification in the passthrough suites; since no default collation exists for
    // whole-db and whole-cluster streams, they can always resume without an explicit collation.
    assertDropCollection(db, collationCollName);
    const doNotModifyInPassthroughs = true;
    assert.commandFailedWithCode(runCommandChangeStreamPassthroughAware(
                                     db,
                                     {
                                       aggregate: collationCollName,
                                       pipeline: [{$changeStream: {resumeAfter: hwmToken}}],
                                       cursor: {}
                                     },
                                     doNotModifyInPassthroughs),
                                 ErrorCodes.InvalidResumeToken);

    // If the client specifies an explicit collation, we are always able to resume.
    assert.commandWorked(db.runCommand({
        aggregate: collationCollName,
        pipeline: [{$changeStream: {resumeAfter: hwmToken}}],
        collation: collationOpts.collation,
        cursor: {}
    }));
})();