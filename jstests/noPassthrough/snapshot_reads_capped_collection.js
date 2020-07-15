/* Test that both transaction and non-transaction snapshot reads on capped collections are not
 * allowed.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();

const collName = "coll";
const primary = replSet.getPrimary();
const primaryDB = primary.getDB('test');

assert.commandWorked(primaryDB.createCollection(collName, {capped: true, size: 32, max: 1}));

// Non-transaction snapshot reads on capped collections are not allowed.
assert.commandFailedWithCode(
    primaryDB.runCommand({find: collName, readConcern: {level: "snapshot"}}),
    ErrorCodes.SnapshotUnavailable);
assert.commandFailedWithCode(
    primaryDB.runCommand(
        {aggregate: collName, pipeline: [], cursor: {}, readConcern: {level: "snapshot"}}),
    ErrorCodes.SnapshotUnavailable);
assert.commandFailedWithCode(
    primaryDB.runCommand({distinct: collName, key: "_id", readConcern: {level: "snapshot"}}),
    ErrorCodes.SnapshotUnavailable);

// After starting a transaction with read concern snapshot, the following find command should fail.
// This is because transaction snapshot reads are banned on capped collections.
const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase('test');
session.startTransaction({readConcern: {level: 'snapshot'}});
assert.commandFailedWithCode(sessionDB.runCommand({find: collName}),
                             ErrorCodes.SnapshotUnavailable);

replSet.stopSet();
})();
