/**
 * Tests that initial sync will continue if the syncing node is removed during syncing.
 * This behavior is desired because transient DNS failures can cause the node to falsely believe
 * that it is removed.
 *
 * @tags: [requires_fcv_47]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const testName = TestData.testName;
const rst = new ReplSetTest({name: testName, nodes: [{}]});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");
const initialSyncSource = primary;

// Add some data to be cloned.
assert.commandWorked(primaryDb.test.insert([{a: 1}, {b: 2}, {c: 3}]));
rst.awaitReplication();

jsTest.log("Adding the initial sync destination node to the replica set");
const initialSyncNode = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
        'logComponentVerbosity': tojson({replication: {verbosity: 1}}),
        'failpoint.forceSyncSourceCandidate':
            tojson({mode: 'alwaysOn', data: {hostAndPort: initialSyncSource.host}})
    }
});
rst.reInitiate();
rst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);

// Set us up to hang before finish so we can check status.
const beforeFinishFailPoint = configureFailPoint(initialSyncNode, "initialSyncHangBeforeFinish");

jsTestLog("Waiting to reach cloning phase of initial sync");
assert.commandWorked(initialSyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCopyingDatabases",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));
jsTestLog("Removing initial sync node");
// Avoid closing the connection when the node transitions to REMOVED.
assert.commandWorked(initialSyncNode.adminCommand({hello: 1, hangUpOnStepDown: false}));

let config = rst.getReplSetConfigFromNode();
const origHost = config.members[1].host;
// This host will never resolve.
config.members[1].host = "always.invalid:27017";
config.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: config, force: 1}));
jsTestLog("Waiting for initial sync node to realize it is removed.");
assert.soonNoExcept(function() {
    assert.commandFailedWithCode(initialSyncNode.adminCommand({replSetGetStatus: 1}),
                                 ErrorCodes.InvalidReplicaSetConfig);
    return true;
});

// Release the initial failpoint.
assert.commandWorked(initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));

jsTestLog("Waiting for initial sync to complete.");
beforeFinishFailPoint.wait();

jsTestLog("Initial sync complete.  Re-adding node to check initial sync status.");
config.members[1].host = origHost;
config.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: config, force: 1}));
rst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);
const res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));
printjson(res.initialSyncStatus);
assert.eq(res.initialSyncStatus.failedInitialSyncAttempts, 0);

jsTestLog("Re-removing node.");
config.members[1].host = "always.invalid:27017";
config.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: config, force: 1}));
jsTestLog("Waiting for initial sync node to realize it is removed again.");
assert.soonNoExcept(function() {
    assert.commandFailedWithCode(initialSyncNode.adminCommand({replSetGetStatus: 1}),
                                 ErrorCodes.InvalidReplicaSetConfig);
    return true;
});

// Add some more data that must be cloned during steady-state replication.
assert.commandWorked(primaryDb.test.insert([{d: 4}, {e: 5}, {f: 6}]));
beforeFinishFailPoint.off();

// Wait until initial sync completion routine is finished.
checkLog.containsJson(initialSyncNode, 4853000);

// Make sure the node is still REMOVED.
assert.commandFailedWithCode(initialSyncNode.adminCommand({replSetGetStatus: 1}),
                             ErrorCodes.InvalidReplicaSetConfig);

jsTestLog("Re-adding initial sync node a final time");
config.members[1].host = origHost;
config.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: config, force: 1}));
rst.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);

rst.stopSet();
})();
