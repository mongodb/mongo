// Makes sure an old heartbeat that is being processed when primary catchup starts does not cause
// primary catchup to think we're already caught up.
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {syncFrom} from "jstests/replsets/rslib.js";

let name = TestData.testName;
let rst = new ReplSetTest({
    name: name,
    nodes: 3,
    // We're not testing catchup takeover in this test, and in the case where primary catchup fails,
    // catchup takeover may cause an additional election and muddle the results.  Setting
    // catchUpTakeoverDelayMillis to -1 disables catchup takeover.
    settings: {chainingAllowed: true, catchUpTakeoverDelayMillis: -1},
    nodeOptions: {
        "setParameter": {
            "logComponentVerbosity": tojsononeline({"replication": {"verbosity": 2}}),
        },
    },
    useBridge: true,
    waitForKeys: true,
});

rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

let primary = rst.getPrimary();
let primaryColl = primary.getDB("test").coll;

// The default WC is majority and this test can't test catchup properly if it used majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

assert(primary.host == rst.nodes[0].host);
// Make us chain node 1 (the node which will become the new primary) from node 2.  Don't allow
// node 1 to switch back.
const forceSyncSource = configureFailPoint(rst.nodes[1], "forceSyncSourceCandidate", {
    "hostAndPort": rst.nodes[2].host,
});
syncFrom(rst.nodes[2], rst.nodes[0], rst);
syncFrom(rst.nodes[1], rst.nodes[2], rst);
const RBIDBeforeStepUp = assert.commandWorked(primary.adminCommand({replSetGetRBID: 1}));

// Disconnect the primary from the node syncing from it.
primary.disconnect(rst.nodes[2]);
// Get a heartbeat from the original primary "stuck" in the new primary.
const newPrimary = rst.nodes[1];
assert.commandWorked(newPrimary.adminCommand({clearLog: "global"}));
let hbfp = configureFailPoint(newPrimary, "pauseInHandleHeartbeatResponse", {"target": primary.host});
hbfp.wait();
// Put the original primary ahead of the secondaries.
assert.commandWorked(primaryColl.insert({_id: 1}));
jsTestLog("Stepping up new primary");
assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
// Allow the "stuck" heartbeat to proceed.
hbfp.off();
// The step-up command waits for the election to complete, but not catch-up. Reconnect the old
// primary to the new primary's sync source to allow replication.
primary.reconnect(rst.nodes[2]);
rst.awaitReplication();
// The new primary should still be primary.
assert.eq(newPrimary.host, rst.getPrimary().host);

// Check if the new primary has any logs with "Member is now in state DOWN". This indicates that it
// failed to connect to a secondary during catchup. If so, it is possible that catchup did not fully
// succeed due to network flakiness.
const newPrimaryFailedToConnect = checkLog.checkContainsWithAtLeastCountJson(newPrimary, 21216, {}, 1);
const RBIDAfterStepUp = assert.commandWorked(primary.adminCommand({replSetGetRBID: 1}));

if (newPrimaryFailedToConnect) {
    // The new primary may not have fully succeeded in catchup.
    assert.lte(RBIDBeforeStepUp.rbid, RBIDAfterStepUp.rbid);
} else {
    // The new primary received all heartbeats successfully. No rollbacks should have happened on
    // the old primary.
    assert.eq(RBIDBeforeStepUp.rbid, RBIDAfterStepUp.rbid);
}
forceSyncSource.off();
rst.stopSet();
