/**
 * Tests that initial sync will abort an attempt if the sync source is removed during cloning.
 * This test will timeout if the attempt is not aborted.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const testName = TestData.testName;
const rst = new ReplSetTest({name: testName, nodes: 1});
const nodes = rst.startSet();
rst.initiateWithHighElectionTimeout();

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
        'numInitialSyncAttempts': 1
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

jsTestLog("Testing removing syncing node in cloner " + cloner + " stage " + stage);
// We can't use remove/reInitiate here because we still need to communicate with the removed
// node.
let config = rst.getReplSetConfig();
config.members.splice(1, 1);  // Removes node[1]
config.version = rst.getReplSetConfigFromNode().version + 1;
assert.commandWorked(primary.getDB("admin").adminCommand({replSetReconfig: config}));

jsTestLog("Waiting for sync node to realize it is removed. It should fail as a result.");
let res;
assert.soon(function() {
    res = checkProgram(initialSyncNode.pid);
    return !res.alive;
});

const fassertProcessExitCode = _isWindows() ? MongoRunner.EXIT_ABRUPT : MongoRunner.EXIT_ABORT;
assert.eq(fassertProcessExitCode, res.exitCode);
assert(
    rawMongoProgramOutput().match('Fatal assertion.*4848002'),
    'Initial syncing node should have crashed as a result of being removed from the configuration.');

// We skip validation and dbhashes because the initial sync failed so the initial sync node is
// unreachable and invalid.
rst.stopSet(null, null, {skipValidation: true});
})();
