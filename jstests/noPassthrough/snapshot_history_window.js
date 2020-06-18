/**
 * Test setting minSnapshotHistoryWindowInSeconds at runtime and that server keeps history for up to
 * minSnapshotHistoryWindowInSeconds.
 *
 * @tags: [requires_majority_read_concern, requires_replication]
 */
(function() {
"use strict";

const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        // Increase log verbosity for storage so we can see how the oldest_timestamp is set.
        setParameter: {logComponentVerbosity: tojson({storage: 2})}
    }
});

replSet.startSet();
replSet.initiate();

const collName = "coll";
const primary = replSet.getPrimary();
const primaryDB = primary.getDB('test');

const historyWindowSecs = 10;
assert.commandWorked(primaryDB.adminCommand(
    {setParameter: 1, minSnapshotHistoryWindowInSeconds: historyWindowSecs}));

const insertTimestamp =
    assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{_id: 0}]}))
        .operationTime;
const startTime = Date.now();
jsTestLog(`Inserted one document at ${insertTimestamp}`);
let nextId = 1;

// Test snapshot window with 1s margin.
const testMarginMS = 1000;

// Test that reading from a snapshot at insertTimestamp is valid for up to historyWindowSecs minus
// the testMarginMS (as a buffer) to avoid races between the client's snapshot read and the update
// of the oldest timestamp in the server.
const testWindowMS = historyWindowSecs * 1000 - testMarginMS;
while (Date.now() - startTime < testWindowMS) {
    // Test that reading from a snapshot at insertTimestamp is still valid.
    assert.commandWorked(primaryDB.runCommand(
        {find: collName, readConcern: {level: "snapshot", atClusterTime: insertTimestamp}}));

    // Perform writes to advance stable timestamp and oldest timestamp. We use majority writeConcern
    // so that we can make sure the stable timestamp and the oldest timestamp are updated after each
    // insert.
    assert.commandWorked(primaryDB.runCommand(
        {insert: collName, documents: [{_id: nextId}], writeConcern: {w: "majority"}}));
    nextId++;

    sleep(50);
}

// Sleep enough to make sure the insertTimestamp falls off the snapshot window.
const historyExpirationTime = startTime + historyWindowSecs * 1000;
sleep(historyExpirationTime + testMarginMS - Date.now());
// Perform another majority write to advance the stable timestamp and the oldest timestamp again.
assert.commandWorked(primaryDB.runCommand(
    {insert: collName, documents: [{_id: nextId}], writeConcern: {w: "majority"}}));

// Test that reading from a snapshot at insertTimestamp returns SnapshotTooOld.
assert.commandFailedWithCode(
    primaryDB.runCommand(
        {find: collName, readConcern: {level: "snapshot", atClusterTime: insertTimestamp}}),
    ErrorCodes.SnapshotTooOld);

// Test that the SnapshotTooOld is recorded in serverStatus.
const serverStatusWT = assert.commandWorked(primaryDB.adminCommand({serverStatus: 1})).wiredTiger;
assert.eq(1,
          serverStatusWT["snapshot-window-settings"]["total number of SnapshotTooOld errors"],
          tojson(serverStatusWT));

replSet.stopSet();
})();
