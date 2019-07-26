/**
 * Test that change streams using speculative majority wait for the latest observed oplog timestamp
 * to majority commit.
 *
 * If a change stream query returns a batch containing oplog entries no newer than timestamp T, the
 * server may still report the latest majority committed oplog timestamp that it observed while
 * scanning the oplog, which may be greater than T. A mongoS will use this timestamp as a guarantee
 * that no new change events will occur at a lesser timestamp. This guarantee is only valid if the
 * timestamp is actually majority committed, so we need to make sure that guarantee holds, even when
 * using speculative majority.
 *
 * @tags: [uses_speculative_majority]
 */
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");  // for [stop|restart]ServerReplication.

const name = "change_stream_speculative_majority_latest_oplog_timestamp";
const replTest = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {enableMajorityReadConcern: 'false'}
});
replTest.startSet();
replTest.initiate();

const dbName = name;
const collName = "coll";
const otherCollName = "coll_other";

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();

const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

assert.commandWorked(primaryColl.insert({_id: 0}, {writeConcern: {w: "majority"}}));

let res = primaryDB.runCommand({
    aggregate: collName,
    pipeline: [{$changeStream: {}}],
    cursor: {},
    maxTimeMS: 5000,
    needsMerge: true,
    fromMongos: true
});

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

// Pause replication on the secondary so that writes won't majority commit.
jsTestLog("Stopping replication to secondary.");
stopServerReplication(secondary);

// Do a write on a collection that we are not watching changes for.
let otherWriteRes = primaryDB.runCommand({insert: otherCollName, documents: [{_id: 1}]});
let otherWriteOpTime = otherWriteRes.operationTime;

// Replication to the secondary is paused, so the write to 'otherCollName' cannot majority
// commit. A change stream getMore is expected to return the "latest oplog timestamp" which it
// scanned and this timestamp must be majority committed. So, this getMore should time out
// waiting for the previous write to majority commit, even though it's on a collection that is
// not being watched.
res = primary.getDB(dbName).runCommand({getMore: cursorId, collection: collName, maxTimeMS: 5000});
assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);

jsTestLog("Restarting replication to secondary.");
restartServerReplication(secondary);
replTest.awaitReplication();

// Now that writes can replicate again, the previous operation should have majority committed,
// making it safe to return as the latest oplog timestamp.
res = primary.getDB(dbName).runCommand({getMore: cursorId, collection: collName, maxTimeMS: 5000});
assert.eq(res.cursor.nextBatch, []);
assert.eq(otherWriteOpTime, res.$_internalLatestOplogTimestamp);

replTest.stopSet();
})();