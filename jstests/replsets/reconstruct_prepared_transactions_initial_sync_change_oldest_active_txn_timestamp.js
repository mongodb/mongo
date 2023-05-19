/**
 * Tests that initial sync successfully reconstructs a prepared transaction even when the sync
 * source started a transaction before the oplog application phase began but after the syncing node
 * fetched the oldest active transaction timestamp.
 *
 * To test this, we pause initial sync after the syncing node has fetched the oldest active
 * transaction timestamp from its sync source. Since there are no in-progress transactions, this
 * value will be null. While paused, the syncing node will start and prepare a transaction and do an
 * additional write. This will mean that when initial sync is unpaused, the syncing node will set
 * the begin applying timestamp to after the prepare timestamp. We can then test that the node still
 * fetches all the oplog entries it needs to reconstruct the prepared transaction.
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

const replTest = new ReplSetTest({nodes: 3});
replTest.startSet();
replTest.initiateWithHighElectionTimeout();

const primary = replTest.getPrimary();
let secondary = replTest.getSecondary();
// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
replTest.awaitReplication();
const dbName = "test";
const collName = jsTestName();
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

const session1 = primary.startSession();
const sessionDB1 = session1.getDatabase(dbName);
const sessionColl1 = sessionDB1.getCollection(collName);

assert.commandWorked(sessionColl1.insert({_id: 1}));

jsTestLog("Restarting the secondary and pausing initial sync");

// Restart the secondary with startClean set to true so that it goes through initial sync. Also
// restart the node with a failpoint turned on that will pause initial sync after it has fetched
// the oldest active transaction timestamp from its sync source to use as the begin fetching
// timestamp. This means we can do some writes on the sync source to change the oldest active
// transaction timestamp before getting the top of the sync source's oplog and make sure the node
// fetches all the oplog entries it needs to reconstruct the prepared transaction.
replTest.stop(secondary);
secondary = replTest.start(secondary,
                           {
                               startClean: true,
                               setParameter: {
                                   'failpoint.initialSyncHangAfterGettingBeginFetchingTimestamp':
                                       tojson({mode: 'alwaysOn'}),
                                   'numInitialSyncAttempts': 1
                               }
                           },
                           true /* wait */);

// Wait for failpoint to be reached so we know that the node is paused. At this point, the
// the beginFetchingTimestamp is Timestamp(0, 0) because there were no open transactions.
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "initialSyncHangAfterGettingBeginFetchingTimestamp",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Preparing a transaction after the initial syncing node fetched the beginFetchingOptime");

session1.startTransaction();
assert.commandWorked(sessionColl1.update({_id: 1}, {_id: 1, a: 1}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session1);

jsTestLog("Inserting another document to advance lastApplied on the sync source");

// This write will advance the last applied timestamp on the sync source, which will mean that
// the beginApplyingTimestamp will be after the prepareTimestamp.
assert.commandWorked(testColl.insert({_id: 2}));

jsTestLog("Resuming initial sync");

assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangAfterGettingBeginFetchingTimestamp", mode: "off"}));

// Wait for the secondary to complete initial sync.
replTest.awaitSecondaryNodes();

jsTestLog("Initial sync completed");

const secondaryColl = secondary.getDB(dbName).getCollection(collName);

jsTestLog("Checking that the first transaction is properly prepared");

// Make sure that we can't read changes to the document from the first prepared transaction
// after initial sync.
assert.docEq({_id: 1}, secondaryColl.findOne({_id: 1}));

jsTestLog("Committing the transaction");

assert.commandWorked(PrepareHelpers.commitTransaction(session1, prepareTimestamp));
replTest.awaitReplication();

// Make sure that we can see the data from a committed transaction on the secondary if it was
// applied during secondary oplog application.
assert.docEq({_id: 1, a: 1}, secondaryColl.findOne({_id: 1}));

replTest.stopSet();
})();
