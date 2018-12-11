/**
 * Tests that the cursor.getResumeToken() shell helper behaves as expected, tracking the resume
 * token with each document and returning the postBatchResumeToken as soon as each batch is
 * exhausted.
 */
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    // Drop and recreate collections to assure a clean run.
    const collName = "change_stream_shell_helper_resume_token";
    const csCollection = assertDropAndRecreateCollection(db, collName);
    const otherCollection = assertDropAndRecreateCollection(db, "unrelated_" + collName);

    const batchSize = 5;

    // Test that getResumeToken() returns the postBatchResumeToken when an empty batch is received.
    const csCursor = csCollection.watch([], {cursor: {batchSize: batchSize}});
    assert(!csCursor.hasNext());
    let curResumeToken = csCursor.getResumeToken();
    assert.neq(undefined, curResumeToken);

    // Test that advancing the oplog time updates the postBatchResumeToken, even with no results.
    assert.commandWorked(otherCollection.insert({}));
    assert(!csCursor.hasNext());  // Causes a getMore to be dispatched.
    let prevResumeToken = curResumeToken;
    curResumeToken = csCursor.getResumeToken();
    assert.neq(undefined, curResumeToken);
    assert.gt(bsonWoCompare(curResumeToken, prevResumeToken), 0);

    // Insert 9 documents into the collection, followed by a write to the unrelated collection.
    for (let i = 0; i < 9; ++i) {
        assert.commandWorked(csCollection.insert({_id: i}));
    }
    assert.commandWorked(otherCollection.insert({}));

    // Retrieve the first batch of 5 events.
    assert(csCursor.hasNext());  // Causes a getMore to be dispatched.
    assert.eq(csCursor.objsLeftInBatch(), batchSize);

    // We have not yet iterated any of the events. Verify that the resume token is unchanged.
    assert.docEq(curResumeToken, csCursor.getResumeToken());

    // For each event in the first batch, the resume token should match the document's _id.
    while (csCursor.objsLeftInBatch()) {
        const nextDoc = csCursor.next();
        prevResumeToken = curResumeToken;
        curResumeToken = csCursor.getResumeToken();
        assert.docEq(curResumeToken, nextDoc._id);
        assert.gt(bsonWoCompare(curResumeToken, prevResumeToken), 0);
    }

    // Retrieve the second batch. This should be 4 documents.
    assert(csCursor.hasNext());  // Causes a getMore to be dispatched.
    assert.eq(csCursor.objsLeftInBatch(), 4);

    // We haven't pulled any events out of the cursor yet, so the resumeToken should be unchanged.
    assert.docEq(curResumeToken, csCursor.getResumeToken());

    // For each of the first 3 events, the resume token should match the document's _id.
    while (csCursor.objsLeftInBatch() > 1) {
        const nextDoc = csCursor.next();
        prevResumeToken = curResumeToken;
        curResumeToken = csCursor.getResumeToken();
        assert.docEq(curResumeToken, nextDoc._id);
        assert.gt(bsonWoCompare(curResumeToken, prevResumeToken), 0);
    }

    // When we pull the final document out of the cursor, the resume token should become the
    // postBatchResumeToken rather than the document's _id. Because we inserted an item into the
    // unrelated collection to push the oplog past the final event returned by the change stream,
    // this will be strictly greater than the final document's _id.
    const finalDoc = csCursor.next();
    prevResumeToken = curResumeToken;
    curResumeToken = csCursor.getResumeToken();
    assert.gt(bsonWoCompare(finalDoc._id, prevResumeToken), 0);
    assert.gt(bsonWoCompare(curResumeToken, finalDoc._id), 0);
}());
