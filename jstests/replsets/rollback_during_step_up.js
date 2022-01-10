/**
 * Tests that step-up to become a primary during rollback won't crash the server with
 * OplogOutOfOrder error after becoming a secondary again.
 * Exercises the fix for SERVER-61977.
 * @tags: [uses_transactions]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/write_concern_util.js");

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: 3,
    settings: {chainingAllowed: false, catchUpTimeoutMillis: 0},
    useBridge: true
});

rst.startSet();
rst.initiateWithHighElectionTimeout();
const node0 = rst.getPrimary();
const node1 = rst.getSecondaries()[0];
const node2 = rst.getSecondaries()[1];
const testDB = node0.getDB('test');
const collName = jsTestName();
const coll = testDB[collName];

assert.commandWorked(testDB.createCollection(coll.getName()));
rst.awaitReplication();

stopServerReplication([node1, node2]);

// Add a non replicated write to the node0 to be rolled back later.
assert.commandWorked(coll.insert({_id: "write 1"}, {writeConcern: {w: 1}}));

let rollbackFp = configureFailPoint(node0, "rollbackHangBeforeTransitioningToRollback");
rst.stepUp(node2, {awaitReplicationBeforeStepUp: false});

// When node0 starts syncing from the node2, it will rollback the write that didn't get replicated.
rollbackFp.wait();

// Step-up node0 while it is hung at rollback.
rst.stepUp(node0, {awaitReplicationBeforeStepUp: false});

// Rollback will fail because we do not allow transitioning to follower mode from node0.
rollbackFp.off();

// Wait for node0 to step-up to avoid InterrupetedDueToReplStateChange errors in the inserts below.
restartServerReplication([node1, node2]);
rst.awaitNodesAgreeOnPrimary(rst.kDefaultTimeoutMS, rst.nodes, node0);
rst.awaitReplication();

// Advance the lastApplied on node0.
assert.commandWorked(coll.insert({_id: "write 2"}));
assert.commandWorked(coll.insert({_id: "write 3"}));
rst.awaitReplication();

// When node0 stepped down it will start syncing again from node2, and it shouldn't crash trying to
// apply entries older than the lastApplied.
rst.stepUp(node2);
rst.awaitNodesAgreeOnPrimary(rst.kDefaultTimeoutMS, rst.nodes, node2);
rst.awaitReplication();

// Verify node0 can replicate new writes.
assert.commandWorked(node2.getDB('test')[collName].insert({_id: "write 4"}));
rst.awaitReplication();

rst.stopSet();
})();
