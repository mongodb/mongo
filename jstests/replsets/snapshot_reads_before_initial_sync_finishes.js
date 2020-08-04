/**
 * Tests that reading at an atClusterTime earlier than the timestamp when initial sync finishes
 * would result in SnapshotTooOld error regardless of the snapshot history window.
 *
 * @tags: [
 *   requires_fcv_47,
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const replSet = new ReplSetTest({nodes: 2});

replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const collName = "coll";
const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
const primaryDB = primary.getDB('test');
const secondaryDB = secondary.getDB('test');

assert.commandWorked(
    primaryDB.runCommand({insert: collName, documents: [{_id: 0}], writeConcern: {w: "majority"}}));

jsTestLog("Adding a new node");
const newNode = replSet.add({
    rsConfig: {priority: 0},
    setParameter: {
        'failpoint.forceSyncSourceCandidate':
            tojson({mode: 'alwaysOn', data: {"hostAndPort": primary.host}}),
        'failpoint.initialSyncHangAfterDataCloning': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
        // Set a large snapshot history window of 1 hour.
        'minSnapshotHistoryWindowInSeconds': 3600,
    }
});
replSet.reInitiate();
replSet.waitForState(newNode, ReplSetTest.State.STARTUP_2);

const newNodeDB = newNode.getDB('test');

assert.commandWorked(newNode.adminCommand({
    waitForFailPoint: "initialSyncHangAfterDataCloning",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Perform another write after the new node finishes cloning.
const timestampDuringInitialSync =
    assert
        .commandWorked(primaryDB.runCommand(
            {insert: collName, documents: [{_id: 1}], writeConcern: {w: "majority"}}))
        .operationTime;
jsTestLog("timestampDuringInitialSync is " + tojson(timestampDuringInitialSync));

// Perform snapshot reads on both the primary and the secondary and test that we can see the
// majority committed writes.
const findAtClusterTimeDuringInitialSync = {
    find: collName,
    readConcern: {level: "snapshot", atClusterTime: timestampDuringInitialSync},
};
const documents = [{_id: 0}, {_id: 1}];
assert.sameMembers(primaryDB.runCommand(findAtClusterTimeDuringInitialSync).cursor.firstBatch,
                   documents);
assert.sameMembers(secondaryDB.runCommand(findAtClusterTimeDuringInitialSync).cursor.firstBatch,
                   documents);

// Test reading at a timestamp before initial sync finishes is not allowed while the node is in
// initial sync.
assert.commandFailedWithCode(newNodeDB.runCommand(findAtClusterTimeDuringInitialSync),
                             ErrorCodes.NotMasterOrSecondary);

// Perform another write so that the new node will finish initial sync at a timestamp higher than
// the timestampDuringInitialSync.
assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{_id: 2}]}));

// Allow the new node to complete initial sync.
assert.commandWorked(
    newNode.adminCommand({configureFailPoint: "initialSyncHangAfterDataCloning", mode: "off"}));
replSet.awaitSecondaryNodes(null, [newNode]);
replSet.awaitLastOpCommitted();

// Test reading at a timestamp before initial sync finishes is not allowed even if the node has
// finished initial sync and has a large snapshot history window size.
assert.commandFailedWithCode(newNodeDB.runCommand(findAtClusterTimeDuringInitialSync),
                             ErrorCodes.SnapshotTooOld);

// Test snapshot readConcern reads all committed writes.
assert.sameMembers(newNodeDB
                       .runCommand({
                           find: collName,
                           readConcern: {level: "snapshot"},
                       })
                       .cursor.firstBatch,
                   [{_id: 0}, {_id: 1}, {_id: 2}]);

replSet.stopSet();
})();
