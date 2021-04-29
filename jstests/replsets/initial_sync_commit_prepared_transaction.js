/**
 * Tests that initial sync successfully applies the commitTransaction oplog entry. To be able to
 * test this, we have to pause collection cloning and run commitTransaction so that the oplog entry
 * is applied during the oplog application phase of initial sync.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */

(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/fail_point_util.js");

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();

const config = replTest.getReplSetConfig();
// Increase the election timeout so that we do not accidentally trigger an election while the
// secondary is restarting.
config.settings = {
    "electionTimeoutMillis": 12 * 60 * 60 * 1000
};
replTest.initiate(config);

const primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

const dbName = "test";
const collName = "initial_sync_commit_prepared_transaction";
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({_id: 1}));

jsTestLog("Preparing a transaction that will be the oldest active transaction");

// Prepare a transaction so that there is an active transaction with an oplog entry. The prepare
// timestamp will become the beginFetchingTimestamp during initial sync.
const session1 = primary.startSession({causalConsistency: false});
const sessionDB1 = session1.getDatabase(dbName);
const sessionColl1 = sessionDB1.getCollection(collName);
session1.startTransaction();
assert.commandWorked(sessionColl1.insert({_id: 2}));
let prepareTimestamp1 = PrepareHelpers.prepareTransaction(session1);

// Do another operation so that the beginFetchingTimestamp will be different from the
// beginApplyingTimestamp.
assert.commandWorked(testColl.insert({_id: 3}));

jsTestLog("Restarting the secondary");

// Restart the secondary with startClean set to true so that it goes through initial sync. Also
// restart the node with a failpoint turned on that will pause initial sync after the secondary
// has copied {_id: 1} and {_id: 3}. This way we can try to commit the prepared transaction
// while initial sync is paused and know that its operations won't be copied during collection
// cloning. Instead, the commitTransaction oplog entry must be applied during oplog application.
replTest.stop(secondary,
              // signal
              undefined,
              // Validation would encounter a prepare conflict on the open transaction.
              {skipValidation: true});
secondary = replTest.start(
    secondary,
    {
        startClean: true,
        setParameter: {
            'failpoint.initialSyncHangDuringCollectionClone': tojson(
                {mode: 'alwaysOn', data: {namespace: testColl.getFullName(), numDocsToClone: 2}}),
            'numInitialSyncAttempts': 1
        }
    },
    true /* wait */);

// Wait for failpoint to be reached so we know that collection cloning is paused.
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "initialSyncHangDuringCollectionClone",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Running operations while collection cloning is paused");

// Commit a transaction on the sync source while collection cloning is paused so that we know
// they must be applied during the oplog application stage of initial sync.
assert.commandWorked(PrepareHelpers.commitTransaction(session1, prepareTimestamp1));

jsTestLog("Resuming initial sync");

// Resume initial sync.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangDuringCollectionClone", mode: "off"}));

// Wait for the secondary to complete initial sync.
replTest.waitForState(secondary, ReplSetTest.State.SECONDARY);

jsTestLog("Initial sync completed");

// Make sure the transaction committed properly and is reflected after the initial sync.
let res = secondary.getDB(dbName).getCollection(collName).findOne({_id: 2});
assert.docEq(res, {_id: 2}, res);

// Step up the secondary after initial sync is done and make sure we can successfully run
// another transaction.
replTest.stepUp(secondary);
replTest.waitForState(secondary, ReplSetTest.State.PRIMARY);
let newPrimary = replTest.getPrimary();
const session2 = newPrimary.startSession({causalConsistency: false});
const sessionDB2 = session2.getDatabase(dbName);
const sessionColl2 = sessionDB2.getCollection(collName);
session2.startTransaction();
assert.commandWorked(sessionColl2.insert({_id: 4}));
let prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2);
assert.commandWorked(PrepareHelpers.commitTransaction(session2, prepareTimestamp2));
res = newPrimary.getDB(dbName).getCollection(collName).findOne({_id: 4});
assert.docEq(res, {_id: 4}, res);

replTest.stopSet();
})();
