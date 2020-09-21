/**
 * Tests that a replica retries on each heartbeat if isSelf fails due to temporary DNS outage during
 * reconfig.
 *
 * @tags: [
 *   requires_fcv_44,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");

const rst = new ReplSetTest(
    {nodes: 1, nodeOptions: {setParameter: {logComponentVerbosity: tojson({replication: 3})}}});
const nodes = rst.startSet();
rst.initiate();

jsTestLog("Add second node to set, with isSelf() disabled by failpoint");

const newNode = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: "failpoint.failIsSelfCheck=" + tojson({mode: "alwaysOn"})
});

rst.reInitiate();

jsTestLog("Await failpoint on second node");

assert.commandWorked(newNode.adminCommand({
    waitForFailPoint: "failIsSelfCheck",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

assert.commandFailedWithCode(nodes[1].adminCommand({replSetGetStatus: 1}),
                             ErrorCodes.NotYetInitialized);

assert.commandWorked(newNode.adminCommand({configureFailPoint: "failIsSelfCheck", mode: "off"}));

jsTestLog("New node re-checks isSelf and becomes secondary");
waitForState(newNode, ReplSetTest.State.SECONDARY);
rst.stopSet();
})();
