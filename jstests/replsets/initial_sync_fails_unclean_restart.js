/**
 * Tests that initial sync will abort an attempt if the sync source restarts from an unclean
 * shutdown. And the sync source node increments its rollback id after the unclean shutdown.
 *
 * This is to test resumable initial sync behavior when the sync source restarts after an unclean
 * shutdown. See SERVER-50140 for more details.
 * @tags: [requires_persistence]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let syncSourceNode = rst.getPrimary();
const syncSourceColl = syncSourceNode.getDB(dbName)[collName];

// Insert some initial data to be cloned.
assert.commandWorked(syncSourceColl.insert([{_id: 1}, {_id: 2}, {_id: 3}]));

jsTest.log("Adding a new node to the replica set");
const initialSyncNode = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
        // Wait for the cloners to finish.
        'failpoint.initialSyncHangAfterDataCloning': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
    }
});
rst.reInitiate();

jsTestLog("The initialSyncNode should hang before the database cloning phase");
checkLog.contains(initialSyncNode, "initialSyncHangBeforeCopyingDatabases fail point enabled");

// Pauses the journal flusher and writes with {j: false}. So this data will be lost after the
// syncSourceNode restarts after an unclean shutdown.
const journalFp = configureFailPoint(syncSourceNode, "pauseJournalFlusherThread");
journalFp.wait();
assert.commandWorked(syncSourceColl.insert({_id: 4}));

// Hang the initialSyncNode before initial sync finishes so we can check initial sync failure.
const beforeFinishFailPoint = configureFailPoint(initialSyncNode, "initialSyncHangBeforeFinish");

jsTestLog("Resuming database cloner on the initialSyncNode");
assert.commandWorked(initialSyncNode.adminCommand(
    {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

jsTestLog("Waiting for data cloning to complete on the initialSyncNode");
assert.commandWorked(initialSyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangAfterDataCloning",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Get the rollback id of the sync source before the unclean shutdown.
const rollbackIdBefore = syncSourceNode.getDB("local").system.rollback.id.findOne();

jsTestLog("Shutting down the syncSourceNode uncleanly");
rst.stop(syncSourceNode,
         9,
         {allowedExitCode: MongoRunner.EXIT_SIGKILL},
         {forRestart: true, waitPid: true});

// Make sure some retries happen due to resumable initial sync and the initial sync does not
// immediately fail while the sync source is completely down.
const nRetries = 2;
checkLog.containsWithAtLeastCount(initialSyncNode, "Trying to reconnect", nRetries);

// Restart the sync source and wait for it to become primary again.
jsTestLog("Restarting the syncSourceNode");
rst.start(syncSourceNode, {waitForConnect: true}, true /* restart */);
syncSourceNode = rst.getPrimary();

// Test that the rollback id is incremented after the unclean shutdown.
const rollbackIdAfter = syncSourceNode.getDB("local").system.rollback.id.findOne();
assert.eq(rollbackIdAfter.rollbackId,
          rollbackIdBefore.rollbackId + 1,
          () => "rollbackIdBefore: " + tojson(rollbackIdBefore) +
              " rollbackIdAfter: " + tojson(rollbackIdAfter));

jsTestLog("Resuming initial sync after the data cloning phase on the initialSyncNode");
assert.commandWorked(initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangAfterDataCloning", mode: "off"}));

jsTestLog("Waiting for initial sync to fail on the initialSyncNode");
beforeFinishFailPoint.wait();
const res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));
// The initial sync should have failed.
assert.eq(res.initialSyncStatus.failedInitialSyncAttempts, 1, () => tojson(res.initialSyncStatus));

// Get rid of the failed node so the fixture can stop properly.
rst.stop(initialSyncNode);
rst.remove(initialSyncNode);

rst.stopSet();
})();
