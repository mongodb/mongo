/**
 * Tests that a replica retries on each heartbeat if isSelf fails due to temporary DNS outage during
 * startup.
 *
 * @tags: [
 *   requires_fcv_47,
 *   requires_persistence
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0, votes: 0}}],
    nodeOptions: {setParameter: {logComponentVerbosity: tojson({replication: 3})}}
});
rst.startSet();
rst.initiate();

const restartNode =
    rst.restart(1, {setParameter: "failpoint.failIsSelfCheck=" + tojson({mode: "alwaysOn"})});

checkLog.contains(
    restartNode,
    "Locally stored replica set configuration does not have a valid entry for the current node; waiting for reconfig or remote heartbeat");
waitForState(restartNode, ReplSetTest.State.REMOVED);

// Clear the log.
const primary = rst.getPrimary();
assert.commandWorked(primary.adminCommand({clearLog: 'global'}));

// Secondary should send heartbeat responses with 'InvalidReplicaSetConfig'.
checkLog.contains(primary, /Received response to heartbeat(.*)InvalidReplicaSetConfig/);

assert.commandWorked(
    restartNode.adminCommand({configureFailPoint: "failIsSelfCheck", mode: "off"}));

// Node 1 re-checks isSelf on next heartbeat and succeeds.
waitForState(restartNode, ReplSetTest.State.SECONDARY);
rst.stopSet();
})();
