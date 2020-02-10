/**
 * Tests that a whole-db or whole-cluster change stream can succeed when the
 * "fullDocumentBeforeChange" option is set to "required", so long as the user
 * specifies a pipeline that filters out changes to any collections which do not
 * have pre-images enabled.
 *
 * @tags: [uses_change_streams, requires_replication]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const testDB = rst.getPrimary().getDB(jsTestName());
const adminDB = rst.getPrimary().getDB("admin");

// Create one collection that has pre-image recording enabled...
const collWithPreImages = testDB.coll_with_pre_images;
assert.commandWorked(testDB.createCollection(collWithPreImages.getName(), {recordPreImages: true}));

//... and one collection which has pre-images disabled.
const collWithNoPreImages = testDB.coll_with_no_pre_images;
assert.commandWorked(
    testDB.createCollection(collWithNoPreImages.getName(), {recordPreImages: false}));

//... and a collection that will hold the sentinal document that marks the end of changes
const sentinelColl = testDB.sentinelColl;

// Insert one document as a starting point and extract its resume token.
const resumeToken = (() => {
    const csCursor = collWithNoPreImages.watch();
    assert.commandWorked(collWithNoPreImages.insert({_id: -1}));
    assert.soon(() => csCursor.hasNext());
    return csCursor.next()._id;
})();

// Write a series of interleaving operations to each collection.
assert.commandWorked(collWithNoPreImages.insert({_id: 0}));
assert.commandWorked(collWithPreImages.insert({_id: 0}));

assert.commandWorked(collWithNoPreImages.update({_id: 0}, {foo: "bar"}));
assert.commandWorked(collWithPreImages.update({_id: 0}, {foo: "bar"}));

assert.commandWorked(collWithNoPreImages.update({_id: 0}, {$set: {foo: "baz"}}));
assert.commandWorked(collWithPreImages.update({_id: 0}, {$set: {foo: "baz"}}));

assert.commandWorked(collWithNoPreImages.remove({_id: 0}));
assert.commandWorked(collWithPreImages.remove({_id: 0}));

// This will generate an insert change event we can wait for on the change stream that indicates
// we have reached the end of changes this test is interested in.
assert.commandWorked(sentinelColl.insert({_id: "last_change_sentinel"}));

// Confirm that attempting to open a whole-db stream on this database with mode "required" fails.
const csWholeDBErr = assert.throws(function() {
    const wholeDBStream =
        testDB.watch([], {fullDocumentBeforeChange: "required", resumeAfter: resumeToken});

    return assert.soon(() => wholeDBStream.hasNext() &&
                           wholeDBStream.next().documentKey._id === "last_change_sentinel");
});
assert.eq(csWholeDBErr.code, 51770);

// Confirm that attempting to open a whole-cluster stream on with mode "required" fails.
const csWholeClusterErr = assert.throws(function() {
    const wholeClusterStream = adminDB.watch([], {
        fullDocumentBeforeChange: "required",
        resumeAfter: resumeToken,
        allChangesForCluster: true,
    });

    return assert.soon(() => wholeClusterStream.hasNext() &&
                           wholeClusterStream.next().documentKey._id == "last_change_sentinel");
});
assert.eq(csWholeClusterErr.code, 51770);

// However, if we open a whole-db or whole-cluster stream that filters for only the namespace with
// pre-images, then the cursor can proceed. This is because the $match gets moved ahead of the
// pre-image lookup stage, so no events from 'collWithNoPreImages' ever reach it, and therefore
// don't trip the validation checks for the existence of the pre-image.
for (let runOnDB of [testDB, adminDB]) {
    // Open a whole-db or whole-cluster stream that filters for the 'collWithPreImages' namespace.
    const csCursor = runOnDB.watch([{$match: {"ns.coll": collWithPreImages.getName()}}], {
        fullDocumentBeforeChange: "required",
        resumeAfter: resumeToken,
        allChangesForCluster: (runOnDB === adminDB)
    });

    // The list of events and pre-images that we expect to see in the stream.
    const expectedPreImageEvents = [
        {opType: "insert", fullDocumentBeforeChange: null},
        {opType: "replace", fullDocumentBeforeChange: {_id: 0}},
        {opType: "update", fullDocumentBeforeChange: {_id: 0, foo: "bar"}},
        {opType: "delete", fullDocumentBeforeChange: {_id: 0, foo: "baz"}}
    ];

    // Confirm that the expected events are all seen, and in the expected order.
    for (let expectedEvent of expectedPreImageEvents) {
        assert.soon(() => csCursor.hasNext());
        const observedEvent = csCursor.next();
        assert.eq(observedEvent.operationType, expectedEvent.opType);
        assert.eq(observedEvent.fullDocumentBeforeChange, expectedEvent.fullDocumentBeforeChange);
    }
}

rst.stopSet();
})();
