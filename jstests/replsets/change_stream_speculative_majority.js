/**
 * Test basic, steady-state replication change stream functionality with speculative majority reads.
 *
 * @tags: [uses_speculative_majority]
 */
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");  // for [stop|restart]ServerReplication.

const name = "change_stream_speculative_majority";
const replTest = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {enableMajorityReadConcern: 'false'}
});
replTest.startSet();
replTest.initiate();

const dbName = name;
const collName = "coll";

let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();
let primaryDB = primary.getDB(dbName);
let primaryColl = primaryDB[collName];

// Open a change stream.
let res = primaryDB.runCommand(
    {aggregate: collName, pipeline: [{$changeStream: {}}], cursor: {}, maxTimeMS: 5000});
assert.commandWorked(res);
let cursorId = res.cursor.id;

// Insert a document on primary and let it majority commit.
assert.commandWorked(primaryColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));

// Receive the first change event.
res = primary.getDB(dbName).runCommand({getMore: cursorId, collection: collName});
let changes = res.cursor.nextBatch;
assert.eq(changes.length, 1);
assert.eq(changes[0]["fullDocument"], {_id: 1});
assert.eq(changes[0]["operationType"], "insert");

// Save the resume token.
let resumeToken = changes[0]["_id"];

// This query should time out waiting for new results and return an empty batch.
res = primary.getDB(dbName).runCommand({getMore: cursorId, collection: collName, maxTimeMS: 5000});
assert.eq(res.cursor.nextBatch, []);

// Pause replication on the secondary so that writes won't majority commit.
stopServerReplication(secondary);

// Do a new write on primary.
assert.commandWorked(primaryColl.insert({_id: 2}));

// The change stream query should time out waiting for the new result to majority commit.
res = primary.getDB(dbName).runCommand({getMore: cursorId, collection: collName, maxTimeMS: 5000});
assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);

// An aggregate trying to resume a stream that includes the change should also time out.
res = primaryDB.runCommand({
    aggregate: collName,
    pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
    cursor: {},
    maxTimeMS: 5000
});
assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);

// Resume the stream after restarting replication. We should now be able to see the new event.
restartServerReplication(secondary);
replTest.awaitReplication();

// Re-open the stream, and receive the new event.
res = primaryDB.runCommand(
    {aggregate: collName, pipeline: [{$changeStream: {resumeAfter: resumeToken}}], cursor: {}});
assert.commandWorked(res);
changes = res.cursor.firstBatch;
assert.eq(changes.length, 1);
assert.eq(changes[0]["fullDocument"], {_id: 2});
assert.eq(changes[0]["operationType"], "insert");

replTest.stopSet();
})();