/**
 * Test that replSetReconfig does not consider non-voting nodes towards the config commitment
 * majority.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isConfigCommitted} from "jstests/replsets/rslib.js";

let replTest = new ReplSetTest({
    nodes: [
        {rsConfig: {priority: 1, votes: 1}},
        {rsConfig: {priority: 0, votes: 1}},
        {rsConfig: {priority: 0, votes: 1}},
        {rsConfig: {priority: 0, votes: 0}},
        {rsConfig: {priority: 0, votes: 0}},
    ],
    useBridge: true,
});
let nodes = replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

// Cause reconfigs via heartbeats to fail on these two nodes, so a config shouldn't be able to
// commit on a majority of voting nodes.
let fp1 = configureFailPoint(nodes[1], "blockHeartbeatReconfigFinish");
let fp2 = configureFailPoint(nodes[2], "blockHeartbeatReconfigFinish");

// Run a reconfig with a timeout of 5 seconds, this should fail with a maxTimeMSExpired error.
jsTestLog("Doing reconfig.");
let config = primary.getDB("local").system.replset.findOne();
config.version++;
assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetReconfig: config, maxTimeMS: 5000}),
    ErrorCodes.MaxTimeMSExpired,
);
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
