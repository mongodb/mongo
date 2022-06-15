// Tests that the $changeStream requires read concern majority.
// @tags: [
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
(function() {
"use strict";

load("jstests/libs/change_stream_util.js");  // For ChangeStreamTest.
load("jstests/libs/namespace_utils.js");     // For getCollectionNameFromFullNamespace.
load("jstests/libs/write_concern_util.js");  // For stopReplicationOnSecondaries.

const rst = new ReplSetTest({nodes: 2, nodeOptions: {enableMajorityReadConcern: ""}});

rst.startSet();
rst.initiate();

const name = "change_stream_require_majority_read_concern";
const db = rst.getPrimary().getDB(name);

// Use ChangeStreamTest to verify that the pipeline returns expected results.
const cst = new ChangeStreamTest(db);

// Attempts to get a document from the cursor with awaitData disabled, and asserts if a
// document is present.
function assertNextBatchIsEmpty(cursor) {
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "alwaysOn"}));
    let res = assert.commandWorked(db.runCommand({
        getMore: cursor.id,
        collection: getCollectionNameFromFullNamespace(cursor.ns),
        batchSize: 1
    }));
    assert.eq(res.cursor.nextBatch.length, 0);
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "off"}));
}

// Test read concerns other than "majority" are not supported.
const primaryColl = db.foo;
assert.commandWorked(primaryColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));
let res = primaryColl.runCommand({
    aggregate: primaryColl.getName(),
    pipeline: [{$changeStream: {}}],
    cursor: {},
    readConcern: {level: "local"},
});
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
res = primaryColl.runCommand({
    aggregate: primaryColl.getName(),
    pipeline: [{$changeStream: {}}],
    cursor: {},
    readConcern: {level: "linearizable"},
});
assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);

// Test that explicit read concern "majority" works.
res = primaryColl.runCommand({
    aggregate: primaryColl.getName(),
    pipeline: [{$changeStream: {}}],
    cursor: {},
    readConcern: {level: "majority"},
});
assert.commandWorked(res);

// Test not specifying readConcern defaults to "majority" read concern.
stopReplicationOnSecondaries(rst);
// Verify that the document just inserted cannot be returned.
let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: primaryColl});
assert.eq(cursor.firstBatch.length, 0);

// Insert a document on the primary only.
assert.commandWorked(primaryColl.insert({_id: 2}, {writeConcern: {w: 1}}));
assertNextBatchIsEmpty(cursor);

// Restart data replicaiton and wait until the new write becomes visible.
restartReplicationOnSecondaries(rst);
rst.awaitLastOpCommitted();

// Verify that the expected doc is returned because it has been committed.
let doc = cst.getOneChange(cursor);
assert.docEq(doc.operationType, "insert");
assert.docEq(doc.fullDocument, {_id: 2});
rst.stopSet();
}());
