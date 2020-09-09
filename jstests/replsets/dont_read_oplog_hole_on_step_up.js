/*
 * Tests that we don't read an oplog hole when we step up while waiting for a tailable oplog query.
 * This test creates a configuration where one secondary, 'secondary', is syncing from a different
 * secondary, 'newPrimary', which is soon to become primary. As the new node becomes primary, the
 * other secondary oplog tailer should not observe any oplog holes.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */
(function() {
'use strict';

load("jstests/replsets/rslib.js");
load("jstests/libs/fail_point_util.js");

var rst = new ReplSetTest({
    name: TestData.testName,
    // The long election timeout results in a 30-second getMore, plenty of time to hit bugs.
    settings: {chainingAllowed: true, electionTimeoutMillis: 60 * 1000},
    nodes: [
        {},
        {},
        {rsConfig: {priority: 0}},
    ],
});
const nodes = rst.startSet();
const oldPrimary = nodes[0];
const newPrimary = nodes[1];
const secondary = nodes[2];

// Make sure this secondary syncs only from the node bound to be the new primary.
assert.commandWorked(secondary.adminCommand({
    configureFailPoint: "forceSyncSourceCandidate",
    mode: "alwaysOn",
    data: {hostAndPort: newPrimary.host}
}));
rst.initiate();

// Make sure when the original primary syncs, it's only from the secondary; this avoids spurious log
// messages.
assert.commandWorked(oldPrimary.adminCommand({
    configureFailPoint: "forceSyncSourceCandidate",
    mode: "alwaysOn",
    data: {hostAndPort: secondary.host}
}));

assert.commandWorked(oldPrimary.getDB(TestData.testName).test.insert({x: 1}));
rst.awaitReplication();

// Force the the secondary tailing the newPrimary to yield its getMore.
const planExecFP = configureFailPoint(newPrimary, "planExecutorHangWhileYieldedInWaitForInserts");

jsTestLog("Stepping up new primary");
assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
rst.awaitNodesAgreeOnPrimary();
assert.eq(newPrimary, rst.getPrimary());

const createCollFP = configureFailPoint(newPrimary, "hangBeforeLoggingCreateCollection");
const createShell = startParallelShell(() => {
    // Implicitly creates the collection.
    assert.commandWorked(db.getSiblingDB(TestData.testName).newcoll.insert({y: 2}));
}, newPrimary.port);

jsTestLog("Waiting for oplog tailer to yield");
planExecFP.wait();

jsTestLog("Waiting for collection creation to hang");
createCollFP.wait();

jsTestLog("Creating hole and resuming oplog tail");
assert.commandWorked(newPrimary.getDB(TestData.testName).test.insert({x: 2}));
planExecFP.off();

// Give enough time for the oplog tailer to resume and observe the oplog hole. The expectation is
// that the secondary oplog tailer should not see any holes. If it does, and misses the collection
// creation oplog entry, then it will crash because it will attempt to apply the insert operation on
// a non-existent namespace. While this specific scenario produces a crash, in general this type of
// bug can introduce data corruption.
sleep(3000);

createCollFP.off();
createShell();

rst.awaitReplication();
rst.stopSet();
}());
