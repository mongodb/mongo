// Tests the behaviour of change streams on an oplog which rolls over.
// @tags: [
//   requires_replication,
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
(function() {
"use strict";

load('jstests/replsets/rslib.js');           // For getLatestOp, getFirstOplogEntry.
load('jstests/libs/change_stream_util.js');  // For ChangeStreamTest.

const oplogSize = 1;  // size in MB
const rst = new ReplSetTest({nodes: 1, oplogSize: oplogSize});

rst.startSet();
rst.initiate();

const testDB = rst.getPrimary().getDB(jsTestName());
const testColl = testDB[jsTestName()];

const cst = new ChangeStreamTest(testDB);

// Write a document to the test collection.
assert.commandWorked(testColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));

let changeStream = cst.startWatchingChanges(
    {pipeline: [{$changeStream: {}}], collection: testColl.getName(), includeToken: true});

// We awaited the replication of the insert, so the change stream shouldn't return them.
assert.commandWorked(testColl.update({_id: 1}, {$set: {updated: true}}));

// Record current time to resume a change stream later in the test.
const resumeTimeFirstUpdate = testDB.runCommand({hello: 1}).$clusterTime.clusterTime;

assert.commandWorked(testColl.update({_id: 1}, {$set: {updated: true}}));

// Test that we see the the update, and remember its resume tokens.
let next = cst.getOneChange(changeStream);
assert.eq(next.operationType, "update");
assert.eq(next.documentKey._id, 1);
const resumeTokenFromFirstUpdate = next._id;

// Write some additional documents, then test we can resume after the first update.
assert.commandWorked(testColl.insert({_id: 2}, {writeConcern: {w: "majority"}}));
assert.commandWorked(testColl.insert({_id: 3}, {writeConcern: {w: "majority"}}));

changeStream = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
    aggregateOptions: {cursor: {batchSize: 0}},
    collection: testColl.getName()
});

for (let nextExpectedId of [2, 3]) {
    assert.eq(cst.getOneChange(changeStream).documentKey._id, nextExpectedId);
}

// Test that the change stream can see additional inserts into the collection.
assert.commandWorked(testColl.insert({_id: 4}, {writeConcern: {w: "majority"}}));
assert.commandWorked(testColl.insert({_id: 5}, {writeConcern: {w: "majority"}}));

for (let nextExpectedId of [4, 5]) {
    assert.eq(cst.getOneChange(changeStream).documentKey._id, nextExpectedId);
}

// Confirm that we can begin a stream at a timestamp that precedes the start of the oplog, if
// the first entry in the oplog is the replica set initialization message.
const firstOplogEntry = getFirstOplogEntry(rst.getPrimary());
assert.eq(firstOplogEntry.o.msg, "initiating set");
assert.eq(firstOplogEntry.op, "n");

const startAtDawnOfTimeStream = cst.startWatchingChanges({
    pipeline: [{$changeStream: {startAtOperationTime: Timestamp(1, 1)}}],
    aggregateOptions: {cursor: {batchSize: 0}},
    collection: testColl.getName()
});

// The first entry we see should be the initial insert into the collection.
const firstStreamEntry = cst.getOneChange(startAtDawnOfTimeStream);
assert.eq(firstStreamEntry.operationType, "insert");
assert.eq(firstStreamEntry.documentKey._id, 1);

// Test that the stream can't resume if the resume token is no longer present in the oplog.

// Roll over the entire oplog such that none of the events are still present.
const primaryNode = rst.getPrimary();
const mostRecentOplogEntry = getLatestOp(primaryNode);
assert.neq(mostRecentOplogEntry, null);
const largeStr = new Array(4 * 1024 * oplogSize).join('abcdefghi');

function oplogIsRolledOver() {
    // The oplog has rolled over if the op that used to be newest is now older than the
    // oplog's current oldest entry. Said another way, the oplog is rolled over when
    // everything in the oplog is newer than what used to be the newest entry.
    return bsonWoCompare(mostRecentOplogEntry.ts,
                         getFirstOplogEntry(primaryNode, {readConcern: "majority"}).ts) < 0;
}

while (!oplogIsRolledOver()) {
    assert.commandWorked(testColl.insert({long_str: largeStr}, {writeConcern: {w: "majority"}}));
}

// Confirm that attempting to continue reading an existing change stream throws CappedPositionLost.
assert.commandFailedWithCode(
    testDB.runCommand({getMore: startAtDawnOfTimeStream.id, collection: testColl.getName()}),
    ErrorCodes.CappedPositionLost);

// Now confirm that attempting to resumeAfter or startAtOperationTime fails.
ChangeStreamTest.assertChangeStreamThrowsCode({
    db: testDB,
    collName: testColl.getName(),
    pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
    expectedCode: ErrorCodes.ChangeStreamHistoryLost
});

ChangeStreamTest.assertChangeStreamThrowsCode({
    db: testDB,
    collName: testColl.getName(),
    pipeline: [{$changeStream: {startAtOperationTime: resumeTimeFirstUpdate}}],
    expectedCode: ErrorCodes.ChangeStreamHistoryLost
});

// We also can't start a stream from the "dawn of time" any more, since the first entry in the
// oplog is no longer the replica set initialization message.
ChangeStreamTest.assertChangeStreamThrowsCode({
    db: testDB,
    collName: testColl.getName(),
    pipeline: [{$changeStream: {startAtOperationTime: Timestamp(1, 1)}}],
    expectedCode: ErrorCodes.ChangeStreamHistoryLost
});

cst.cleanUp();
rst.stopSet();
})();
