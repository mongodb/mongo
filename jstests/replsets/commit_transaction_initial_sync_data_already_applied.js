/**
 * Tests that initial sync successfully applies the commitTransaction oplog entry even if the data
 * already reflects the transaction and that initial sync will apply each operation from the
 * transaction in separate storage transactions.
 *
 * We pause initial sync before any collection cloning and run commitTransaction so that the data
 * will reflect the transaction when the commitTransaction oplog entry is applied during the oplog
 * application phase of initial sync. The transaction is set up so that when applied, one of its
 * operations will fail, but a later operation will need to succeed to pass the test. This will show
 * that operations from a transaction are executed in separate storage transactions.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
load("jstests/core/txns/libs/prepare_helpers.js");

const replTest = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

const dbName = "test";
const collName = "commit_transaction_initial_sync_data_already_applied";
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({_id: 1, a: 0}));

// Ensure that the "a" field is unique
assert.commandWorked(testColl.createIndex({"a": 1}, {unique: true}));

jsTestLog("Restarting the secondary");

// Restart the secondary with startClean set to true so that it goes through initial sync. Also
// restart the node with a failpoint turned on that will pause initial sync before cloning any
// collections, but during the period that the sync source is fetching oplog entries from the
// sync source. This will make it so that all operations after this and before the failpoint is
// turned off will be reflected in the data but also applied during the oplog application phase
// of initial sync.
secondary = replTest.restart(secondary, {
    startClean: true,
    setParameter: {
        'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1
    }
});

// Wait for fail point message to be logged so that we know that initial sync is paused.
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCopyingDatabases",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Initial sync paused");

const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

assert.commandWorked(testColl.update({_id: 1}, {_id: 1, a: 0, b: 0}));

session.startTransaction();

// When the commitTransaction oplog entry is applied, this operation should fail with a
// duplicate key error because the data will already reflect the transaction.
assert.commandWorked(sessionColl.insert({_id: 2, a: 1}));

// When the commitTransaction oplog entry is applied, this operation should succeed even though
// the one before it fails. This is used to make sure that initial sync is applying operations
// from a transaction in a separate storage transaction.
assert.commandWorked(sessionColl.update({_id: 1}, {$unset: {b: 1}}));

assert.commandWorked(sessionColl.update({_id: 2}, {$unset: {a: 1}}));
assert.commandWorked(sessionColl.insert({_id: 3, a: 1}));

jsTestLog("Preparing and committing a transaction");

let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

// Resume initial sync.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));

// Wait for the secondary to complete initial sync.
replTest.waitForState(secondary, ReplSetTest.State.SECONDARY);

jsTestLog("Initial sync completed");

// Make sure that the later operations from the transaction succeed even though the first
// operation will fail during oplog application.
let res = secondary.getDB(dbName).getCollection(collName).find();
assert.eq(res.toArray(), [{_id: 1, a: 0}, {_id: 2}, {_id: 3, a: 1}], res);

replTest.stopSet();
})();
