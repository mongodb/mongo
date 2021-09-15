// Tests the behaviour of $changeStream's 'fullDocument' option when retrieving point-in-time
// post-images.
// @tags: [multiversion_incompatible]
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
load("jstests/libs/change_stream_util.js");        // For isChangeStreamPreAndPostImagesEnabled.

const testDB = db.getSiblingDB(jsTestName());
const coll = assertDropAndRecreateCollection(testDB, "test");

if (!isChangeStreamPreAndPostImagesEnabled(db)) {
    // If feature flag is off, creating changeStream with new fullDocument arguments should throw.
    assert.throwsWithCode(() => coll.watch([], {fullDocument: 'whenAvailable'}),
                          ErrorCodes.BadValue);
    assert.throwsWithCode(() => coll.watch([], {fullDocument: 'required'}), ErrorCodes.BadValue);

    jsTestLog(
        'Skipping test because featureFlagChangeStreamPreAndPostImages feature flag is not enabled');
    return;
}

// Open the change streams with new fullDocument parameters.
assert.doesNotThrow(() => coll.watch([], {fullDocument: 'whenAvailable'}));
assert.doesNotThrow(() => coll.watch([], {fullDocument: 'required'}));
}());
