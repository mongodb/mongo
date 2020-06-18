/**
 * Test that a change stream on the primary node survives stepdown.
 *
 * Change streams are only supported on WiredTiger.
 * @tags: [requires_wiredtiger]
 */
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");  // for [stop|restart]ServerReplication.
load("jstests/libs/parallel_shell_helpers.js");

const name = "change_stream_stepdown";
const replTest = new ReplSetTest({name: name, nodes: [{}, {}]});
replTest.startSet();
// Initiate with high election timeout to prevent any election races.
replTest.initiateWithHighElectionTimeout();

function stepUp(replTest, conn) {
    assert.commandWorked(conn.adminCommand({replSetFreeze: 0}));
    // Steps up the node in conn but this function does not wait for the new primary to be able to
    // accept writes.
    replTest.stepUpNoAwaitReplication(conn);
    // Waits for the new primary to accept new writes.
    return replTest.getPrimary();
}

const dbName = name;
const collName = "change_stream_stepdown";
const changeStreamComment = collName + "_comment";

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();
const primaryDb = primary.getDB(dbName);
const secondaryDb = secondary.getDB(dbName);
const primaryColl = primaryDb[collName];

// Open a change stream.
let res = primaryDb.runCommand({
    aggregate: collName,
    pipeline: [{$changeStream: {}}],
    cursor: {},
    comment: changeStreamComment,
    maxTimeMS: 5000
});
assert.commandWorked(res);
let cursorId = res.cursor.id;

// Insert several documents on primary and let them majority commit.
assert.commandWorked(
    primaryColl.insert([{_id: 1}, {_id: 2}, {_id: 3}], {writeConcern: {w: "majority"}}));
replTest.awaitReplication();

jsTestLog("Testing that changestream survives stepdown between find and getmore");
// Step down.
assert.commandWorked(primaryDb.adminCommand({replSetStepDown: 60, force: true}));
replTest.waitForState(primary, ReplSetTest.State.SECONDARY);

// Receive the first change event.  This tests stepdown between find and getmore.
res = assert.commandWorked(
    primaryDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));
let changes = res.cursor.nextBatch;
assert.eq(changes.length, 1);
assert.eq(changes[0]["fullDocument"], {_id: 1});
assert.eq(changes[0]["operationType"], "insert");

jsTestLog("Testing that changestream survives step-up");
// Step back up and wait for primary.
stepUp(replTest, primary);

// Get the next one.  This tests that changestreams survives a step-up.
res = assert.commandWorked(
    primaryDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));
changes = res.cursor.nextBatch;
assert.eq(changes.length, 1);
assert.eq(changes[0]["fullDocument"], {_id: 2});
assert.eq(changes[0]["operationType"], "insert");

jsTestLog("Testing that changestream survives stepdown between two getmores");
// Step down again.
assert.commandWorked(primaryDb.adminCommand({replSetStepDown: 60, force: true}));
replTest.waitForState(primary, ReplSetTest.State.SECONDARY);

// Get the next one.  This tests that changestreams survives a step down between getmores.
res = assert.commandWorked(
    primaryDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));
changes = res.cursor.nextBatch;
assert.eq(changes.length, 1);
assert.eq(changes[0]["fullDocument"], {_id: 3});
assert.eq(changes[0]["operationType"], "insert");

// Step back up and wait for primary.
stepUp(replTest, primary);

jsTestLog("Testing that changestream waiting on old primary sees docs inserted on new primary");

replTest.awaitReplication();  // Ensure secondary is up to date and can win an election.

function shellFn(secondaryHost, dbName, collName, changeStreamComment, stepUpFn) {
    // Wait for the getMore to be in progress.
    assert.soon(() => db.getSiblingDB("admin")
                          .aggregate([
                              {'$currentOp': {}},
                              {
                                  '$match': {
                                      op: 'getmore',
                                      'cursor.originatingCommand.comment': changeStreamComment
                                  }
                              }
                          ])
                          .itcount() == 1);

    const replTest = new ReplSetTest(secondaryHost);
    const secondary = new Mongo(secondaryHost);
    const secondaryDb = secondary.getDB(dbName);
    // Step down the old primary and wait for new primary.
    jsTestLog(`Stepping up ${secondaryHost} and waiting for new primary`);
    stepUpFn(replTest, secondary);

    jsTestLog("Inserting document on new primary");
    assert.commandWorked(secondaryDb[collName].insert({_id: 4}), {writeConcern: {w: "majority"}});
}
let waitForShell = startParallelShell(
    funWithArgs(shellFn, secondary.host, dbName, collName, changeStreamComment, stepUp),
    primary.port);

res = assert.commandWorked(primaryDb.runCommand({
    getMore: cursorId,
    collection: collName,
    batchSize: 1,
    maxTimeMS: ReplSetTest.kDefaultTimeoutMS
}));
changes = res.cursor.nextBatch;
assert.eq(changes.length, 1);
assert.eq(changes[0]["fullDocument"], {_id: 4});
assert.eq(changes[0]["operationType"], "insert");

waitForShell();

replTest.stopSet();
})();
