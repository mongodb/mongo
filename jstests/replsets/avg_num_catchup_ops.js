/**
 * Tests that the election metric for average number of catchup ops is being set correctly. We test
 * this by electing a node to be primary twice and forcing it to catch up each time.
 */
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/election_metrics.js");
load("jstests/replsets/rslib.js");

const name = jsTestName();
const rst = new ReplSetTest(
    {name: name, nodes: 3, useBridge: true, settings: {catchUpTimeoutMillis: 4 * 60 * 1000}});

rst.startSet();
rst.initiateWithHighElectionTimeout();
rst.awaitSecondaryNodes();

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(rst.getPrimary().adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
rst.awaitReplication();

const testNode = rst.getSecondaries()[0];

let stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp(rst, testNode);
restartServerReplication(stepUpResults.oldSecondaries);
// Block until the primary finishes drain mode.
assert.eq(stepUpResults.newPrimary, rst.getPrimary());
// Wait until the new primary completes the transition to primary and writes a no-op.
checkLog.contains(stepUpResults.newPrimary, "Transition to primary complete");

let testNodeReplSetGetStatus =
    assert.commandWorked(stepUpResults.newPrimary.adminCommand({replSetGetStatus: 1}));
let testNodeServerStatus =
    assert.commandWorked(stepUpResults.newPrimary.adminCommand({serverStatus: 1}));

// Check that metrics associated with catchup have been set correctly in both replSetGetStatus and
// serverStatus.
assert(testNodeReplSetGetStatus.electionCandidateMetrics.numCatchUpOps,
       () => "Response should have an 'numCatchUpOps' field: " +
           tojson(testNodeReplSetGetStatus.electionCandidateMetrics));
// numCatchUpOps should be 4 because the 'foo' collection is implicitly created during the 3
// inserts, and that's where the additional oplog entry comes from.
assert.eq(testNodeReplSetGetStatus.electionCandidateMetrics.numCatchUpOps, 4);
assert(testNodeServerStatus.electionMetrics.averageCatchUpOps,
       () => "Response should have an 'averageCatchUpOps' field: " +
           tojson(testNodeServerStatus.electionMetrics));
assert.eq(testNodeServerStatus.electionMetrics.averageCatchUpOps, 4);

// Step up another node temporarily.
const tempPrimary = rst.stepUp(rst.getSecondaries()[0]);
rst.awaitReplication();

// Step up the testNode and force it to catchup again.
stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp(rst, testNode);
restartServerReplication(stepUpResults.oldSecondaries);
assert.eq(stepUpResults.newPrimary, rst.getPrimary());
checkLog.contains(stepUpResults.newPrimary, "Transition to primary complete");
rst.awaitReplication();

testNodeServerStatus =
    assert.commandWorked(stepUpResults.newPrimary.adminCommand({serverStatus: 1}));
testNodeReplSetGetStatus =
    assert.commandWorked(stepUpResults.newPrimary.adminCommand({replSetGetStatus: 1}));

// numCatchUpOps is now 3 due to the 'foo' collection already being created.
assert.eq(testNodeReplSetGetStatus.electionCandidateMetrics.numCatchUpOps, 3);
assert.eq(testNodeServerStatus.electionMetrics.numCatchUps, 2);
assert.eq(testNodeServerStatus.electionMetrics.averageCatchUpOps, 3.5);

rst.stopSet();
})();
