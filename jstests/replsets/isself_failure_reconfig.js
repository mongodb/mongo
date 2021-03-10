/**
 * Tests that a replica retries on each heartbeat if isSelf fails due to temporary DNS outage during
 * reconfig.
 *
 * @tags: [
 *   requires_fcv_47,
 * ]
 */

(function() {
    "use strict";

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

    checkLog.contains(newNode, "failIsSelfCheck failpoint activated, returning false from isSelf");

    assert.commandFailedWithCode(nodes[1].adminCommand({replSetGetStatus: 1}),
                                 ErrorCodes.NotYetInitialized);

    assert.commandWorked(
        newNode.adminCommand({configureFailPoint: "failIsSelfCheck", mode: "off"}));

    jsTestLog("New node re-checks isSelf and becomes secondary");
    waitForState(newNode, ReplSetTest.State.SECONDARY);
    rst.stopSet();
})();
