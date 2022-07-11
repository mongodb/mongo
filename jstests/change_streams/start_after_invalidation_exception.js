// Test cases to verify the resumabilty of the change streams when the '$match' predicate is
// specified which filters out the invalidate event.
// @tags: [do_not_run_in_whole_cluster_passthrough]

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

const testDB = db.getSiblingDB("change_stream_check_resumability");
const collName = "test";
const coll = assertDropAndRecreateCollection(testDB, collName);

// Open the change streams for the 'insert' operation type.
let cursor = coll.watch([{$match: {operationType: 'insert'}}]);

// Test that upon insertion we get a batch with one element.
assert.commandWorked(coll.insert({_id: 0, a: 1}));
assert.soon(() => cursor.hasNext());
let next = cursor.next();
assert.eq(next.operationType, "insert");
assert.docEq(next.fullDocument, {_id: 0, a: 1});

// Drop the database, this will cause invalidation of the change streams.
assert.commandWorked(testDB.dropDatabase());

// Confirm that we do not see the invalidation event, but the stream is closed.
assert.soon(() => {
    assert(!cursor.hasNext());
    return cursor.isExhausted();
});

// Retrieve the final resume token in the stream, which should be the invalidate token.
const invalidateResumeToken = cursor.getResumeToken();

// Recreate the collection and insert a new document.
assertCreateCollection(testDB, collName);
assert.commandWorked(coll.insert({_id: 1, a: 101}));

// Start a new change stream after the invalidation, with the same $match filter which only matches
// "insert" events.
cursor = coll.watch([{$match: {operationType: 'insert'}}], {startAfter: invalidateResumeToken});

// Verify that despite the fact that the stream filters out "invalidate" events, we are nonetheless
// able to start after the invalidation and can see the insert on the recreated collection.
assert.soon(() => cursor.hasNext());
next = cursor.next();
assert.eq(next.operationType, "insert");
assert.docEq(next.fullDocument, {_id: 1, a: 101});
}());
