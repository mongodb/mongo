/**
 * "This test is supposed to reproduce an invariant failure where a node that is stepping down due
 * to a new term will hit an invariant during a scheduled automatic reconfig to remove a
 * "newlyAdded" field.
 * 1. Start a 3 node replica set
 * 2. Enable a fail point that hangs during automatic reconfig
 * 3. Add another node (nodes[3]) to the set
 * 4. Wait till the above fail point is reached
 * 5. Enable a fail point that blocks a heartbeat caused stepdown before updating the term
 * 6. Cause a stepdown by disconnecting two secondary nodes from the primary
 * 7. Wait for the node nodes[1] to step up and become primary as the only one having non-0
 * priority)
 * 8. Wait until stepdown fail point from (5) is reached.
 * 9. Release the auto reconfig failpoint to make auto reconfig proceed and reach the new check that
 * verifies there is no pending term update and fail because of it
 * 10. Make sure the failure took place via checking logs.
 * 11. Make sure "newlyAdded" for a nodes[3] field is still being removed by the new primary via log
 * check
 * 12. Release a stepdown fail point
 * @tags: [
 *     requires_fcv_82
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = jsTestName();
const dbName = "testdb";
const collName = "testcoll";

const rst =
    new ReplSetTest({name: testName, nodes: [{}, {}, {rsConfig: {priority: 0}}], useBridge: true});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();

const primaryDb = primary.getDB(dbName);

const hangBeforeNewlyAddedRemovalFP = configureFailPoint(primaryDb, "hangDuringAutomaticReconfig");
// We want to ensure log message 4634503 is printed and it is a debug level message
assert.commandWorked(primary.setLogLevel(2));
jsTestLog("Adding one more node to the replica set");
const secondary3 = rst.add({
    rsConfig: {priority: 0},
});
rst.reInitiate();
rst.awaitSecondaryNodes(null, [secondary3]);

// Waiting for auto reconfig to block
hangBeforeNewlyAddedRemovalFP.wait();
const nodes = rst.nodes;
const configBeforeTermBump = assert.commandWorked(primaryDb.adminCommand({replSetGetConfig: 1}));
assert.eq(1, configBeforeTermBump.config.term, () => tojson(configBeforeTermBump));
rst.awaitReplication();
assert.eq(nodes[0], primary);
function waitForNewPrimary() {
    assert.soon(function() {
        return nodes[1].adminCommand('hello').isWritablePrimary;
    });
}

// Stop the secondaries from replicating so that the primary steps down.
const blockHeartbeatStepdownFailPoint = configureFailPoint(nodes[0], 'blockHeartbeatStepdown');
nodes[1].disconnect(nodes[0]);
nodes[2].disconnect(nodes[0]);
jsTestLog("Waiting for a new primary");
waitForNewPrimary();
nodes[1].reconnect(nodes[0]);
nodes[2].reconnect(nodes[0]);
// Blocking stepdown process, so we can run reconfig while stepdown is pending
blockHeartbeatStepdownFailPoint.wait();
// Let auto reconfig continue while step down is blocked
hangBeforeNewlyAddedRemovalFP.off();
jsTestLog("Proceeding with newlyAdded field removal");
// "Wait until the new primary successfully removes the 'newlyAdded' field for node 3. The old
// primary should fail the automatic reconfig to remove the 'newlyAdded' field due to the pending
// term update on stepdown."

checkLog.contains(primary, 'Safe reconfig rejected due to detected pending stepdown');
checkLog.containsJson(nodes[1], 4634504, {"memberId": 3});
blockHeartbeatStepdownFailPoint.off();
rst.stopSet();