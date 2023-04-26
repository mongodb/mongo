/**
 * Test that replSetReconfig waits for a majority of voting nodes to commit all oplog
 * entries from the previous config in the current config.
 */

(function() {
"use strict";

load("jstests/replsets/rslib.js");
load("jstests/libs/write_concern_util.js");

// Start a 3 node replica set with two non-voting nodes. In this case, only one node is
// needed to satisfy the oplog commitment check.
var replTest = new ReplSetTest({
    nodes: [
        {rsConfig: {priority: 1, votes: 1}},
        {rsConfig: {priority: 0, votes: 0}},
        {rsConfig: {priority: 0, votes: 0}},
    ]
});
var nodes = replTest.startSet();

// Stopping replication on secondaries can be very slow with a high election timeout. Set a small
// oplog getMore timeout so the test runs faster.
nodes.forEach(node => {
    assert.commandWorked(
        node.adminCommand({configureFailPoint: 'setSmallOplogGetMoreMaxTimeMS', mode: 'alwaysOn'}));
});

replTest.initiateWithHighElectionTimeout();
var primary = replTest.getPrimary();

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
replTest.awaitReplication();
// Do a write that should not be able to replicate to node1 since we stopped replication.
stopServerReplication(nodes[1]);
assert.commandWorked(primary.getDB("test")["test"].insert({x: 1}));

// Run a reconfig that changes node1's votes to 1. The reconfig succeeds when it replicates
// to a majority of nodes.
jsTestLog("Doing reconfig.");
var config = primary.getDB("local").system.replset.findOne();
config.version++;
config.members[1].votes = 1;
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

// Node1 must replicate the previous write in order for a current reconfig to succeed.
// This new reconfig has a timeout of 5 seconds, and should fail with a CurrentConfigNotCommittedYet
// error.
config.version++;
assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config, maxTimeMS: 5000}),
                             ErrorCodes.CurrentConfigNotCommittedYet);
// Check the latest reconfig is rejected.
assert.gt(config.version, replTest.getReplSetConfigFromNode().version);
restartServerReplication(nodes[1]);

replTest.awaitReplication();

replTest.stopSet();
}());
