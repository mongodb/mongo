/**
 * Test that an erroneous Change Stream pipeline responds with an error that includes the
 * "NonResumableChangeStreamError" label.
 */

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

// Drop and recreate the collections to be used in this set of tests.
const coll = assertDropAndRecreateCollection(db, "change_stream_error_label");

// Attaching a projection to the Change Stream that filters out the resume token (stored in the
// _id field) guarantees a ChangeStreamFatalError as soon as we get the first change.
const changeStream = coll.watch([{$project: {_id: 0}}], {batchSize: 1});
assert.commandWorked(coll.insert({a: 1}));

const err = assert.throws(function() {
    // Call hasNext() until it throws an error or unexpectedly returns true. We need the
    // assert.soon() to keep trying here, because the above insert command isn't immediately
    // observable to the change stream in sharded configurations.
    assert.soon(function() {
        return changeStream.hasNext();
    }, undefined, undefined, undefined, {runHangAnalyzer: false});
});

// The hasNext() sends a getMore command, which should generate a ChangeStreamFatalError reply
// that includes the NonResumableChangeStreamError errorLabel.
assert.commandFailedWithCode(err, ErrorCodes.ChangeStreamFatalError);
assert("errorLabels" in err, err);
assert.contains("NonResumableChangeStreamError", err.errorLabels, err);
}());
