/**
 * Tests that a replica retries on each heartbeat if isSelf fails due to temporary DNS outage during
 * startup.
 *
 * @tags: [
 *   requires_fcv_44,
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

// "Locally stored replica set configuration does not have a valid entry for the current node".
checkLog.containsJson(restartNode, 21405);
waitForState(restartNode, ReplSetTest.State.REMOVED);

// "Received response to heartbeat".
checkLog.containsJson(rst.getPrimary(), 4615620, {
    response: (response) => {
        return response.codeName === "InvalidReplicaSetConfig";
    }
});

assert.commandWorked(
    restartNode.adminCommand({configureFailPoint: "failIsSelfCheck", mode: "off"}));

// Node 1 re-checks isSelf on next heartbeat and succeeds.
waitForState(restartNode, ReplSetTest.State.SECONDARY);
rst.stopSet();
})();
