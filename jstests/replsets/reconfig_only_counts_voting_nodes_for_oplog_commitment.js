/*
 * Test that replSetReconfig waits for a majority of voting nodes to commit all oplog
 * entries from the previous config in the current config.
 *
 * @tags: [requires_fcv_44]
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
replTest.initiate();
var primary = replTest.getPrimary();
var secondary = replTest.getSecondary();

// Do a write that should not be able to replicate to node1 since we stopped replication.
stopServerReplication(nodes[1]);
assert.commandWorked(primary.getDB("test")["test"].insert({x: 1}));

// Run a reconfig that changes node1's votes to 1. This means that node1 must replicate
// the previous write in order for a new reconfig to succeed.
// This reconfig has a timeout of 5 seconds, and should fail with a maxTimeMSExpired error.
jsTestLog("Doing reconfig.");
var config = primary.getDB("local").system.replset.findOne();
config.version++;
config.members[1].votes = 1;
assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetReconfig: config, maxTimeMS: 5000}),
    ErrorCodes.MaxTimeMSExpired);

restartServerReplication(nodes[1]);

replTest.awaitReplication();

replTest.stopSet();
}());