/**
 * Tests that initial sync will abort an attempt if the sync source is removed during cloning.
 * This test will timeout if the attempt is not aborted.
 * @tags: [live_record_incompatible]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const testName = "initial_sync_fails_when_source_removed";
const rst = new ReplSetTest({name: testName, nodes: [{}, {rsConfig: {priority: 0}}]});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");
const initialSyncSource = rst.getSecondary();

// Add some data to be cloned.
assert.commandWorked(primaryDb.test.insert([{a: 1}, {b: 2}, {c: 3}]));
rst.awaitReplication();

jsTest.log("Adding the initial sync destination node to the replica set");
const initialSyncNode = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
        'failpoint.forceSyncSourceCandidate':
            tojson({mode: 'alwaysOn', data: {hostAndPort: initialSyncSource.host}})
    }
});
rst.reInitiate();
rst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);

// The code handling this case is common to all cloners, so run it only for the stage most likely
// to see an error.
const cloner = 'CollectionCloner';
const stage = 'query';

// Set us up to hang before finish so we can check status.
const beforeFinishFailPoint = configureFailPoint(initialSyncNode, "initialSyncHangBeforeFinish");
const initialSyncNodeDb = initialSyncNode.getDB("test");
const failPointData = {
    cloner: cloner,
    stage: stage,
    nss: 'test.test'
};
// Set us up to stop right before the given stage.
const beforeStageFailPoint =
    configureFailPoint(initialSyncNodeDb, "hangBeforeClonerStage", failPointData);
// Release the initial failpoint.
assert.commandWorked(initialSyncNodeDb.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));
beforeStageFailPoint.wait();

jsTestLog("Testing removing sync source in cloner " + cloner + " stage " + stage);
// Avoid closing the connection when the node transitions to REMOVED.
assert.commandWorked(initialSyncNode.adminCommand({hello: 1, hangUpOnStepDown: false}));
// We can't use remove/reInitiate here because that does not properly remove a node
// in the middle of a config.
let config = rst.getReplSetConfig();
config.members.splice(1, 1);  // Removes node[1]
config.version = rst.getReplSetConfigFromNode().version + 1;
assert.commandWorked(primary.getDB("admin").adminCommand({replSetReconfig: config}));

jsTestLog("Waiting for source to realize it is removed.");
assert.soonNoExcept(() => assert.commandFailedWithCode(
                        initialSyncSource.getDB("test").adminCommand({replSetGetStatus: 1}),
                        ErrorCodes.InvalidReplicaSetConfig));

jsTestLog("Resuming the initial sync.");
beforeStageFailPoint.off();
beforeFinishFailPoint.wait();
const res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));
// The initial sync should have failed.
assert.eq(res.initialSyncStatus.failedInitialSyncAttempts, 1);
beforeFinishFailPoint.off();

// We skip validation and dbhashes because the initial sync failed so the initial sync node is
// invalid and unreachable.
TestData.skipCheckDBHashes = true;
rst.stopSet(null, null, {skipValidation: true});
})();
