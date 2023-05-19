/**
 * Test that replSetReconfig does not consider non-voting nodes towards the config commitment
 * majority.
 */

(function() {
"use strict";

load("jstests/replsets/rslib.js");
load("jstests/libs/fail_point_util.js");

var replTest = new ReplSetTest({
    nodes: [
        {rsConfig: {priority: 1, votes: 1}},
        {rsConfig: {priority: 0, votes: 1}},
        {rsConfig: {priority: 0, votes: 1}},
        {rsConfig: {priority: 0, votes: 0}},
        {rsConfig: {priority: 0, votes: 0}}
    ],
    useBridge: true
});
var nodes = replTest.startSet();
replTest.initiateWithHighElectionTimeout();
var primary = replTest.getPrimary();
var secondary = replTest.getSecondary();

// Cause reconfigs via heartbeats to fail on these two nodes, so a config shouldn't be able to
// commit on a majority of voting nodes.
let fp1 = configureFailPoint(nodes[1], "blockHeartbeatReconfigFinish");
let fp2 = configureFailPoint(nodes[2], "blockHeartbeatReconfigFinish");

// Run a reconfig with a timeout of 5 seconds, this should fail with a maxTimeMSExpired error.
jsTestLog("Doing reconfig.");
var config = primary.getDB("local").system.replset.findOne();
config.version++;
assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetReconfig: config, maxTimeMS: 5000}),
    ErrorCodes.MaxTimeMSExpired);
assert.eq(isConfigCommitted(primary), false);

// Turn off failpoints so that heartbeat reconfigs on the voting nodes can succeed.
fp1.off();
fp2.off();
assert.soon(() => isConfigCommitted(primary));

// Subsequent reconfig should now succeed.
config.version++;
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: config}));
assert.soon(() => isConfigCommitted(primary));

replTest.stopSet();
}());
