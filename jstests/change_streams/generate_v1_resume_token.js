/**
 * Test that the $_generateV2ResumeTokens parameter can be used to force change streams to return v1
 * tokens.
 * @tags: [
 *   requires_fcv_61
 * ]
 */
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

const coll = assertDropAndRecreateCollection(db, jsTestName());

// Create one stream that returns v2 tokens, the default.
const v2Stream = coll.watch([]);

// Create a second stream that explicitly requests v1 tokens.
const v1Stream = coll.watch([], {$_generateV2ResumeTokens: false});

// Insert a test document into the collection.
assert.commandWorked(coll.insert({_id: 1}));

// Wait until both streams have encountered the insert operation.
assert.soon(() => v1Stream.hasNext() && v2Stream.hasNext());
const v1Event = v1Stream.next();
const v2Event = v2Stream.next();

// Confirm that the streams see the same event, but the resume tokens differ.
const v1ResumeToken = v1Event._id;
const v2ResumeToken = v2Event._id;

delete v1Event._id;
delete v2Event._id;

assert.docEq(v1Event, v2Event);
assert.neq(v1ResumeToken, v2ResumeToken, {v1ResumeToken, v2ResumeToken});
})();
