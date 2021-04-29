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
    ],
});
const nodes = rst.startSet();
// Initiate in two steps so that the first two nodes finish initial sync before the third begins
// its initial sync. This prevents the long getMore timeout from causing the first initial sync to
// take so much time that the second cannot succeed.
rst.initiate();

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(rst.getPrimary().adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

const oldPrimary = nodes[0];
const newPrimary = nodes[1];
const secondary = rst.add({rsConfig: {priority: 0}});

// Make sure this secondary syncs only from the node bound to be the new primary.
assert.commandWorked(secondary.adminCommand({
    configureFailPoint: "forceSyncSourceCandidate",
    mode: "alwaysOn",
    data: {hostAndPort: newPrimary.host}
}));
rst.reInitiate();

// Wait for all 'newlyAdded' field removals to prevent auto reconfigs from interfering with the
// replSetStepUp command below.
rst.waitForAllNewlyAddedRemovals();

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

// Wait for the node to transition to primary and accept writes.
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
