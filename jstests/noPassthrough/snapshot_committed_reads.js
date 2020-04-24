// Test that non-transaction snapshot reads with atClusterTime (or afterClusterTime) will wait for
// the majority commit point to move past the atClusterTime (or afterClusterTime) before they can
// read.
// @tags: [requires_replication, requires_majority_read_concern]
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");  // For stopReplicationOnSecondaries.

const replSet = new ReplSetTest({nodes: 2});

replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const collName = "coll";
const primary = replSet.getPrimary();
const primaryDB = primary.getDB('test');

// Stop replication.
stopReplicationOnSecondaries(replSet);

// Perform a write and get its operation timestamp.
let writeTimestamp =
    assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{x: 1}]}))
        .operationTime;

// Test that snapshot read with atClusterTime times out waiting for commit point to advance.
assert.commandFailedWithCode(primaryDB.runCommand({
    find: collName,
    readConcern: {level: "snapshot", atClusterTime: writeTimestamp},
    maxTimeMS: 1000,
}),
                             ErrorCodes.MaxTimeMSExpired);

// Test that snapshot read with afterClusterTime times out waiting for commit point to advance.
assert.commandFailedWithCode(primaryDB.runCommand({
    find: collName,
    readConcern: {level: "snapshot", afterClusterTime: writeTimestamp},
    maxTimeMS: 1000,
}),
                             ErrorCodes.MaxTimeMSExpired);

// Restart replication.
restartReplicationOnSecondaries(replSet);
replSet.awaitLastOpCommitted();

// Test that snapshot read with atClusterTime works after replication resumes.
assert.commandWorked(primaryDB.runCommand({
    find: collName,
    readConcern: {level: "snapshot", atClusterTime: writeTimestamp},
    maxTimeMS: 1000,
}));

// Test that snapshot read with afterClusterTime works after replication resumes.
assert.commandWorked(primaryDB.runCommand({
    find: collName,
    readConcern: {level: "snapshot", afterClusterTime: writeTimestamp},
    maxTimeMS: 1000,
}));

replSet.stopSet();
}());
