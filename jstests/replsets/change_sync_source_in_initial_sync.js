/**
 * Tests that calling 'replSetSyncFrom' on an initial syncing node will cancel the current syncing
 * attempt and cause it to retry against the newly designated sync source.
 *
 * @tags: [requires_fcv_44]
 */

(function() {
"use strict";

const testName = "change_sync_source_in_initial_sync";
const dbName = testName;

const rst = new ReplSetTest({
    name: testName,
    nodes: [{}, {rsConfig: {priority: 0}}],
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const secondary = rst.getSecondary();
const collName = "testColl";
const primaryColl = primaryDB[collName];

assert.commandWorked(primaryColl.insert({_id: "a"}, {writeConcern: {w: 2}}));

// Add a third node to the replica set, force it to sync from the primary, and have it hang in the
// middle of initial sync.
const initialSyncNode = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
        'failpoint.forceSyncSourceCandidate':
            tojson({mode: 'alwaysOn', data: {hostAndPort: primary.name}})
    }
});
rst.reInitiate();
rst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);

let res = assert.commandWorked(initialSyncNode.adminCommand({"replSetGetStatus": 1}));
assert.eq(primary.name, res.syncSourceHost, res);
assert.eq(0, res.initialSyncStatus.failedInitialSyncAttempts);
assert.commandWorked(
    initialSyncNode.adminCommand({configureFailPoint: "forceSyncSourceCandidate", mode: "off"}));

jsTestLog("Setting the initial sync source from secondary to primary.");
assert.commandWorked(initialSyncNode.adminCommand({replSetSyncFrom: secondary.name}));

// Turning off the 'initialSyncHangBeforeCopyingDatabases' failpoint should cause initial sync
// to restart with the secondary as the sync source.
let hangBeforeFinishInitialSync =
    configureFailPoint(initialSyncNode, "initialSyncHangBeforeFinish");
assert.commandWorked(initialSyncNode.adminCommand(
    {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));
hangBeforeFinishInitialSync.wait();
res = assert.commandWorked(initialSyncNode.adminCommand({"replSetGetStatus": 1}));
assert.eq(secondary.name, res.syncSourceHost, res);
assert.eq(1, res.initialSyncStatus.failedInitialSyncAttempts);
assert.eq(2, res.initialSyncStatus.initialSyncAttempts.length);
hangBeforeFinishInitialSync.off();

rst.awaitSecondaryNodes();
rst.stopSet();
})();