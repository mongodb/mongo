/**
 * Tests that the cursor.getResumeToken() shell helper behaves as expected, tracking the resume
 * token with each document and returning the postBatchResumeToken as soon as each batch is
 * exhausted.
 * @tags: [
 *   requires_replication,
 *   requires_majority_read_concern,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

// Create a new single-node replica set, and ensure that it can support $changeStream.
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB(jsTestName());
const collName = "change_stream_shell_helper_resume_token";
const csCollection = assertDropAndRecreateCollection(db, collName);
const otherCollection = assertDropAndRecreateCollection(db, "unrelated_" + collName);

const batchSize = 5;
let docId = 0;

// Test that getResumeToken() returns the postBatchResumeToken when an empty batch is received.
const csCursor = csCollection.watch([], {cursor: {batchSize: batchSize}});
assert(!csCursor.hasNext());
let curResumeToken = csCursor.getResumeToken();
assert.neq(undefined, curResumeToken);

// Test that advancing the oplog time updates the postBatchResumeToken, even with no results.
assert.commandWorked(otherCollection.insert({}));
let prevResumeToken = curResumeToken;
assert.soon(() => {
    assert(!csCursor.hasNext());  // Causes a getMore to be dispatched.
    prevResumeToken = curResumeToken;
    curResumeToken = csCursor.getResumeToken();
    assert.neq(undefined, curResumeToken);
    return bsonWoCompare(curResumeToken, prevResumeToken) > 0;
});

// Insert 9 documents into the collection, followed by a write to the unrelated collection.
for (let i = 0; i < 9; ++i) {
    assert.commandWorked(csCollection.insert({_id: ++docId}));
}
assert.commandWorked(otherCollection.insert({}));

// Retrieve the first batch of events from the cursor.
assert.soon(() => csCursor.hasNext());  // Causes a getMore to be dispatched.

// We have not yet iterated any of the events. Verify that the resume token is unchanged.
assert.docEq(curResumeToken, csCursor.getResumeToken());

// For each event in the first batch, the resume token should match the document's _id.
let currentDoc = null;
while (csCursor.objsLeftInBatch()) {
    currentDoc = csCursor.next();
    prevResumeToken = curResumeToken;
    curResumeToken = csCursor.getResumeToken();
    assert.docEq(curResumeToken, currentDoc._id);
    assert.gt(bsonWoCompare(curResumeToken, prevResumeToken), 0);
}

// Retrieve the second batch of events from the cursor.
assert.soon(() => csCursor.hasNext());  // Causes a getMore to be dispatched.

// We haven't pulled any events out of the cursor yet, so the resumeToken should be unchanged.
assert.docEq(curResumeToken, csCursor.getResumeToken());

// For all but the final event, the resume token should match the document's _id.
while ((currentDoc = csCursor.next()).fullDocument._id < docId) {
    assert.soon(() => csCursor.hasNext());
    prevResumeToken = curResumeToken;
    curResumeToken = csCursor.getResumeToken();
    assert.docEq(curResumeToken, currentDoc._id);
    assert.gt(bsonWoCompare(curResumeToken, prevResumeToken), 0);
}
// When we reach here, 'currentDoc' is the final document in the batch, but we have not yet
// updated the resume token. Assert that this resume token sorts before currentDoc's.
prevResumeToken = curResumeToken;
assert.gt(bsonWoCompare(currentDoc._id, prevResumeToken), 0);

// After we have pulled the final document out of the cursor, the resume token should be the
// postBatchResumeToken rather than the document's _id. Because we inserted an item into the
// unrelated collection to push the oplog past the final event returned by the change stream,
// this will be strictly greater than the final document's _id.
assert.soon(() => {
    curResumeToken = csCursor.getResumeToken();
    assert(!csCursor.hasNext(), () => tojson(csCursor.next()));
    return bsonWoCompare(curResumeToken, currentDoc._id) > 0;
});

rst.stopSet();
}());
