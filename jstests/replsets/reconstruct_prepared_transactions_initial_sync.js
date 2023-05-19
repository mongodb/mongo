/**
 * Tests that initial sync successfully reconstructs multiple prepared transactions during the oplog
 * application phase of initial sync and that we can commit or abort the transactions afterwards.
 * It also tests that after reconstructing a prepared transaction at the end of initial sync, we can
 * successfully apply a commitTransaction oplog entry during secondary oplog application. To be able
 * to test this, we have to pause collection cloning and run operations on the sync source so that
 * we have oplog entries to apply during the oplog application phase of initial sync. We will also
 * prepare a transaction while collection cloning is paused to test applying a prepare oplog entry
 * during oplog application phase of initial sync.
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
const collName = "reconstruct_prepared_transactions_initial_sync";
let testDB = primary.getDB(dbName);
let testColl = testDB.getCollection(collName);

const session1 = primary.startSession();
const sessionDB1 = session1.getDatabase(dbName);
const sessionColl1 = sessionDB1.getCollection(collName);

let session2 = primary.startSession();
let sessionDB2 = session2.getDatabase(dbName);
const sessionColl2 = sessionDB2.getCollection(collName);

let session3 = primary.startSession();
let sessionDB3 = session3.getDatabase(dbName);
const sessionColl3 = sessionDB3.getCollection(collName);

assert.commandWorked(sessionColl1.insert({_id: 1}));
assert.commandWorked(sessionColl2.insert({_id: 2}));
assert.commandWorked(sessionColl3.insert({_id: 3}));
assert.commandWorked(sessionColl3.insert({_id: 4}));

jsTestLog("Preparing three transactions");

session1.startTransaction();
assert.commandWorked(sessionColl1.update({_id: 1}, {_id: 1, a: 1}));
const prepareTimestamp1 = PrepareHelpers.prepareTransaction(session1);

session2.startTransaction();
assert.commandWorked(sessionColl2.update({_id: 2}, {_id: 2, a: 1}));
let prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2);

session3.startTransaction();
assert.commandWorked(sessionColl3.update({_id: 3}, {_id: 3, a: 1}));
const prepareTimestamp3 = PrepareHelpers.prepareTransaction(session3);

const lsid2 = session2.getSessionId();
const txnNumber2 = session2.getTxnNumber_forTesting();

const lsid3 = session3.getSessionId();
const txnNumber3 = session3.getTxnNumber_forTesting();

jsTestLog("Restarting the secondary");

// Restart the secondary with startClean set to true so that it goes through initial sync. Also
// restart the node with a failpoint turned on that will pause initial sync after the secondary
// has copied {_id: 1}, {_id: 2} and {_id: 3}. This way we can do some writes on the sync source
// while initial sync is paused and know that its operations won't be copied during collection
// cloning. Instead, the writes must be applied during oplog application.
replTest.stop(secondary, undefined /* signal */, {skipValidation: true});
secondary = replTest.start(
    secondary,
    {
        startClean: true,
        setParameter: {
            'failpoint.initialSyncHangDuringCollectionClone': tojson(
                {mode: 'alwaysOn', data: {namespace: testColl.getFullName(), numDocsToClone: 3}}),
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

// Perform writes while collection cloning is paused so that we know they must be applied during
// the oplog application stage of initial sync.
assert.commandWorked(testColl.insert({_id: 5}));

let session4 = primary.startSession();
let sessionDB4 = session4.getDatabase(dbName);
const sessionColl4 = sessionDB4.getCollection(collName);

jsTestLog("Preparing the fourth transaction");

// Prepare a transaction while collection cloning is paused so that its oplog entry must be
// applied during the oplog application phase of initial sync.
session4.startTransaction();
assert.commandWorked(sessionColl4.update({_id: 4}, {_id: 4, a: 1}));
const prepareTimestamp4 = PrepareHelpers.prepareTransaction(session4, {w: 1});

jsTestLog("Resuming initial sync");

// Resume initial sync.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangDuringCollectionClone", mode: "off"}));

// Wait for the secondary to complete initial sync.
replTest.awaitSecondaryNodes();
// Wait for the commit point to advance past the "prepare" oplog entry for transaction four.
replTest.awaitLastOpCommitted();

jsTestLog("Initial sync completed");

secondary.setSecondaryOk();
const secondaryColl = secondary.getDB(dbName).getCollection(collName);

// Make sure that while reading from the node that went through initial sync, we can't read
// changes to the documents from any of the prepared transactions after initial sync. Also, make
// sure that the writes that happened when collection cloning was paused happened.
const res = secondaryColl.find().sort({_id: 1}).toArray();
assert.eq(res, [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}], res);

jsTestLog("Checking that the first transaction is properly prepared");

// Make sure that we can't read changes to the document from the first prepared transaction
// after initial sync.
assert.docEq({_id: 1}, secondaryColl.findOne({_id: 1}));

jsTestLog("Committing the first transaction");

assert.commandWorked(PrepareHelpers.commitTransaction(session1, prepareTimestamp1));
replTest.awaitReplication();

// Make sure that we can see the data from a committed transaction on the secondary if it was
// applied during secondary oplog application.
assert.docEq({_id: 1, a: 1}, secondaryColl.findOne({_id: 1}));

jsTestLog("Checking that the fourth transaction is properly prepared");

// Make sure that we can't read changes to the document from the first prepared transaction
// after initial sync.
assert.docEq({_id: 4}, secondaryColl.findOne({_id: 4}));

jsTestLog("Committing the fourth transaction");

assert.commandWorked(PrepareHelpers.commitTransaction(session4, prepareTimestamp4));
replTest.awaitReplication();

// Make sure that we can see the data from a committed transaction on the secondary if it was
// applied during secondary oplog application.
assert.docEq({_id: 4, a: 1}, secondaryColl.findOne({_id: 4}));

jsTestLog("Stepping up the secondary");

// Step up the secondary after initial sync is done and make sure the other two transactions are
// properly prepared.
const newPrimary = replTest.stepUp(secondary, {awaitReplicationBeforeStepUp: false});
testDB = newPrimary.getDB(dbName);
testColl = testDB.getCollection(collName);

// Force the second session to use the same lsid and txnNumber as from before the restart. This
// ensures that we're working with the same session and transaction.
session2 = PrepareHelpers.createSessionWithGivenId(newPrimary, lsid2);
session2.setTxnNumber_forTesting(txnNumber2);
sessionDB2 = session2.getDatabase(dbName);

jsTestLog("Checking that the second transaction is properly prepared");

// Make sure that we can't read changes to the document from the second prepared transaction
// after initial sync.
assert.eq(testColl.find({_id: 2}).toArray(), [{_id: 2}]);

// Make sure that another write on the same document from the second transaction causes a write
// conflict.
assert.commandFailedWithCode(
    testDB.runCommand(
        {update: collName, updates: [{q: {_id: 2}, u: {$set: {a: 2}}}], maxTimeMS: 5 * 1000}),
    ErrorCodes.MaxTimeMSExpired);

// Make sure that we cannot add other operations to the second transaction since it is prepared.
assert.commandFailedWithCode(sessionDB2.runCommand({
    insert: collName,
    documents: [{_id: 6}],
    txnNumber: NumberLong(txnNumber2),
    stmtId: NumberInt(2),
    autocommit: false
}),
                             ErrorCodes.PreparedTransactionInProgress);

jsTestLog("Committing the second transaction");

// Make sure we can successfully commit the second transaction after recovery.
assert.commandWorked(sessionDB2.adminCommand({
    commitTransaction: 1,
    commitTimestamp: prepareTimestamp2,
    txnNumber: NumberLong(txnNumber2),
    autocommit: false
}));
assert.eq(testColl.find({_id: 2}).toArray(), [{_id: 2, a: 1}]);

jsTestLog("Attempting to run another transaction");

// Make sure that we can run another conflicting transaction without any problems
session2.startTransaction();
assert.commandWorked(sessionDB2[collName].update({_id: 2}, {_id: 2, a: 3}));
prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2);
assert.commandWorked(PrepareHelpers.commitTransaction(session2, prepareTimestamp2));
assert.docEq({_id: 2, a: 3}, testColl.findOne({_id: 2}));

// Force the third session to use the same lsid and txnNumber as from before the restart. This
// ensures that we're working with the same session and transaction.
session3 = PrepareHelpers.createSessionWithGivenId(newPrimary, lsid3);
session3.setTxnNumber_forTesting(txnNumber3);
sessionDB3 = session3.getDatabase(dbName);

jsTestLog("Checking that the third transaction is properly prepared");

// Make sure that we can't read changes to the document from the third prepared transaction
// after initial sync.
assert.eq(testColl.find({_id: 3}).toArray(), [{_id: 3}]);

// Make sure that another write on the same document from the third transaction causes a write
// conflict.
assert.commandFailedWithCode(
    testDB.runCommand(
        {update: collName, updates: [{q: {_id: 3}, u: {$set: {a: 2}}}], maxTimeMS: 5 * 1000}),
    ErrorCodes.MaxTimeMSExpired);

// Make sure that we cannot add other operations to the third transaction since it is prepared.
assert.commandFailedWithCode(sessionDB3.runCommand({
    insert: collName,
    documents: [{_id: 6}],
    txnNumber: NumberLong(txnNumber3),
    stmtId: NumberInt(2),
    autocommit: false
}),
                             ErrorCodes.PreparedTransactionInProgress);

jsTestLog("Aborting the third transaction");

// Make sure we can successfully abort the third transaction after recovery.
assert.commandWorked(sessionDB3.adminCommand(
    {abortTransaction: 1, txnNumber: NumberLong(txnNumber3), autocommit: false}));
assert.eq(testColl.find({_id: 3}).toArray(), [{_id: 3}]);

replTest.stopSet();
})();
