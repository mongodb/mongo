// Tests the behaviour of an agg with $_requestReshardingResumeToken on an oplog which rolls over.
// @tags: [
//   requires_replication,
//   requires_majority_read_concern,
// ]
(function() {
"use strict";

load('jstests/replsets/rslib.js');  // For getLatestOp, getFirstOplogEntry.

const oplogSize = 1;  // size in MB
const rst = new ReplSetTest({nodes: 1, oplogSize: oplogSize});

rst.startSet();
rst.initiate();

const testDB = rst.getPrimary().getDB(jsTestName());
const testColl = testDB[jsTestName()];

const localDB = testDB.getSiblingDB("local");
const oplogColl = localDB.oplog.rs;

// Insert one document into the test collection.
const insertCmdRes = assert.commandWorked(testDB.runCommand(
    {insert: testColl.getName(), documents: [{_id: 1}], writeConcern: {w: "majority"}}));

// Record the optime of the insert to resume from later in the test.
const resumeTimeFirstInsert = insertCmdRes.operationTime;

// Update the document to create another oplog entry.
assert.commandWorked(testColl.update({_id: 1}, {$set: {updated: true}}));

// Verify that an aggregation which requests a resharding resume token but does not include a filter
// on 'ts' is rejected.
assert.commandFailedWithCode(localDB.runCommand({
    aggregate: oplogColl.getName(),
    pipeline: [{$match: {ns: testColl.getFullName()}}],
    cursor: {},
    $_requestReshardingResumeToken: true
}),
                             ErrorCodes.InvalidOptions);

// Verify that we can start an aggregation from the timestamp that we took earlier, and that we see
// the subsequent update operation.
let aggCmdRes = assert.commandWorked(localDB.runCommand({
    aggregate: oplogColl.getName(),
    pipeline: [{$match: {ts: {$gt: resumeTimeFirstInsert}, ns: testColl.getFullName()}}],
    cursor: {},
    $_requestReshardingResumeToken: true
}));

const aggCmdCursor = new DBCommandCursor(localDB, aggCmdRes);
assert.soon(() => aggCmdCursor.hasNext());
let next = aggCmdCursor.next();
assert.eq(next.op, "u");
assert.eq(next.o2._id, 1);

// Confirm that we can begin an aggregation at a timestamp that precedes the start of the oplog, if
// the first entry in the oplog is the replica set initialization message.
const firstOplogEntry = getFirstOplogEntry(rst.getPrimary());
assert.eq(firstOplogEntry.o.msg, "initiating set");
assert.eq(firstOplogEntry.op, "n");

aggCmdRes = assert.commandWorked(localDB.runCommand({
    aggregate: oplogColl.getName(),
    pipeline: [{$match: {ts: {$gte: Timestamp(1, 1)}, ns: testColl.getFullName()}}],
    cursor: {},
    $_requestReshardingResumeToken: true
}));
const startAtDawnOfTimeCursor = new DBCommandCursor(localDB, aggCmdRes);

for (let expectedOp of [{op: "i", _id: 1}, {op: "u", _id: 1}]) {
    assert.soon(() => startAtDawnOfTimeCursor.hasNext());
    next = startAtDawnOfTimeCursor.next();
    assert.eq(next.op, expectedOp.op);
    assert.eq((next.o._id || next.o2._id), expectedOp._id);
}

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

// Test that attempting to start from a timestamp that has already fallen off the oplog fails, if we
// specify $_requestReshardingResumeToken.
assert.commandFailedWithCode(localDB.runCommand({
    aggregate: oplogColl.getName(),
    pipeline: [{$match: {ts: {$gte: resumeTimeFirstInsert}, ns: testColl.getFullName()}}],
    cursor: {},
    $_requestReshardingResumeToken: true
}),
                             ErrorCodes.OplogQueryMinTsMissing);

assert.commandFailedWithCode(localDB.runCommand({
    aggregate: oplogColl.getName(),
    pipeline: [{$match: {ts: {$gte: Timestamp(1, 1)}, ns: testColl.getFullName()}}],
    cursor: {},
    $_requestReshardingResumeToken: true
}),
                             ErrorCodes.OplogQueryMinTsMissing);

// However, the same aggregation succeeds if we do not specify $_requestReshardingResumeToken. Since
// we have just rolled over the oplog, we may encounter a "CappedPositionLost" error, which can be
// safely retried.
assert.soon(() => {
    let commandRes = localDB.runCommand({
        aggregate: oplogColl.getName(),
        pipeline: [{$match: {ts: {$gte: Timestamp(1, 1)}, ns: testColl.getFullName()}}],
        cursor: {},
    });

    if (!commandRes.ok && commandRes.code == ErrorCodes.CappedPositionLost) {
        jsTestLog("Encountered a CappedPositionLost error, retrying the command.");
        return false;
    }

    assert.commandWorked(commandRes);
    return true;
});

// Requesting resume tokens on a find command does not imply 'assertMinTsHasNotFallenOffOplog'.
// Since we have just rolled over the oplog, we may encounter a "CappedPositionLost" error, which
// can be safely retried.
assert.soon(() => {
    let commandRes = localDB.runCommand({
        find: oplogColl.getName(),
        filter: {ts: {$gte: Timestamp(1, 1)}, ns: testColl.getFullName()},
        tailable: true,
        hint: {$natural: 1},
        $_requestResumeToken: true
    });

    if (!commandRes.ok && commandRes.code == ErrorCodes.CappedPositionLost) {
        jsTestLog("Encountered a CappedPositionLost error, retrying the command.");
        return false;
    }

    assert.commandWorked(commandRes);
    return true;
});

rst.stopSet();
})();
