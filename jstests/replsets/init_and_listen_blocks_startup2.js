/**
 * Tests that a replica set node does not transition from STARTUP to STARTUP2
 * until _initAndListen completes. See SERVER-113405.
 * @tags: [
 *   requires_fcv_83,
 *   requires_persistence,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({name: jsTestName(), nodes: 1});
rst.startSet();
rst.initiate();
assert.commandWorked(rst.getPrimary().getDB("test").coll.insert({x: 1}));

// Restart with failpoint that blocks signalInitAndListenComplete.
const restartNode = rst.restart(0, {
    setParameter: "failpoint.hangBeforeNotifyStorageStartupRecoveryComplete=" + tojson({mode: "alwaysOn"}),
});

// Wait for node to reach the failpoint (transport layer ready, but signal blocked)
assert.commandWorked(
    restartNode.adminCommand({
        waitForFailPoint: "hangBeforeNotifyStorageStartupRecoveryComplete",
        timesEntered: 1,
        maxTimeMS: 60000,
    }),
);

// Verify node has NOT transitioned to STARTUP2 while failpoint is active
assert(
    !checkLog.checkContainsOnce(restartNode, '"newState":"STARTUP2"'),
    "Node should not transition to STARTUP2 until _initAndListen completes",
);

jsTestLog("Verified: STARTUP2 blocked while failpoint active");

// Disable failpoint to allow signal, which will unblock the wait
assert.commandWorked(
    restartNode.adminCommand({configureFailPoint: "hangBeforeNotifyStorageStartupRecoveryComplete", mode: "off"}),
);

// Verify node transitions to STARTUP2 after signal is sent
assert.soon(
    () => checkLog.checkContainsOnce(restartNode, '"newState":"STARTUP2"'),
    "Node should transition to STARTUP2 after signal is sent",
);

rst.waitForState(restartNode, [ReplSetTest.State.PRIMARY, ReplSetTest.State.SECONDARY]);

rst.stopSet();
