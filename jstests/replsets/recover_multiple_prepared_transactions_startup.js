/**
 * Test that startup recovery successfully recovers multiple prepared transactions and that we can
 * commit or abort the transaction afterwards.
 *
 * @tags: [requires_persistence, uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();

const dbName = "test";
const collName = "recover_multiple_prepared_transactions_startup";
let testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

testDB.runCommand({drop: collName});
assert.commandWorked(testDB.runCommand({create: collName}));

let session = primary.startSession({causalConsistency: false});
let sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

let session2 = primary.startSession({causalConsistency: false});
let sessionDB2 = session2.getDatabase(dbName);
const sessionColl2 = sessionDB2.getCollection(collName);

assert.commandWorked(sessionColl.insert({_id: 1}));
assert.commandWorked(sessionColl2.insert({_id: 2}));

jsTestLog("Disable snapshotting on all nodes");

// Disable snapshotting so that future operations do not enter the majority snapshot.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "disableSnapshotting", mode: "alwaysOn"}));

session.startTransaction();
assert.commandWorked(sessionColl.update({_id: 1}, {_id: 1, a: 1}));
let prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: 1});

session2.startTransaction();
assert.commandWorked(sessionColl2.update({_id: 2}, {_id: 2, a: 1}));
let prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2, {w: 1});

const lsid = session.getSessionId();
const txnNumber = session.getTxnNumber_forTesting();

const lsid2 = session2.getSessionId();
const txnNumber2 = session2.getTxnNumber_forTesting();

jsTestLog("Restarting node");

// Perform a clean shutdown and restart. Note that the 'disableSnapshotting' failpoint will be
// unset on the node following the restart.
replTest.stop(primary, undefined, {skipValidation: true});
replTest.start(primary, {}, true);

jsTestLog("Node was restarted");

primary = replTest.getPrimary();
testDB = primary.getDB(dbName);

session = primary.startSession({causalConsistency: false});
sessionDB = session.getDatabase(dbName);

session2 = primary.startSession({causalConsistency: false});
sessionDB2 = session.getDatabase(dbName);

// Force the first session to use the same lsid and txnNumber as from before the restart. This
// ensures that we're working with the same session and transaction.
session._serverSession.handle.getId = () => lsid;
session.setTxnNumber_forTesting(txnNumber);

jsTestLog("Checking that the first transaction is properly prepared");

// Make sure that we can't read changes to the document from the first transaction after
// recovery.
assert.eq(testDB[collName].find({_id: 1}).toArray(), [{_id: 1}]);

// Make sure that another write on the same document from the first transaction causes a write
// conflict.
assert.commandFailedWithCode(
    testDB.runCommand(
        {update: collName, updates: [{q: {_id: 1}, u: {$set: {a: 2}}}], maxTimeMS: 5 * 1000}),
    ErrorCodes.MaxTimeMSExpired);

// Make sure that we cannot add other operations to the first transaction since it is prepared.
assert.commandFailedWithCode(sessionDB.runCommand({
    insert: collName,
    documents: [{_id: 3}],
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(2),
    autocommit: false
}),
                             ErrorCodes.PreparedTransactionInProgress);

jsTestLog("Committing the first transaction");

// Make sure we can successfully commit the first transaction after recovery.
let commitTimestamp = Timestamp(prepareTimestamp.getTime(), prepareTimestamp.getInc() + 1);
assert.commandWorked(sessionDB.adminCommand({
    commitTransaction: 1,
    commitTimestamp: commitTimestamp,
    txnNumber: NumberLong(txnNumber),
    autocommit: false
}));

// Force the second session to use the same lsid and txnNumber as from before the restart.
// This ensures that we're working with the same session and transaction.
session._serverSession.handle.getId = () => lsid2;
session.setTxnNumber_forTesting(txnNumber2);

jsTestLog("Checking that the second transaction is properly prepared");

// Make sure that we can't read changes to the document from the second transaction after
// recovery.
assert.eq(testDB[collName].find({_id: 2}).toArray(), [{_id: 2}]);

// Make sure that another write on the same document from the second transaction causes a write
// conflict.
assert.commandFailedWithCode(
    testDB.runCommand(
        {update: collName, updates: [{q: {_id: 2}, u: {$set: {a: 2}}}], maxTimeMS: 5 * 1000}),
    ErrorCodes.MaxTimeMSExpired);

// Make sure that we cannot add other operations to the second transaction since it is prepared.
assert.commandFailedWithCode(sessionDB2.runCommand({
    insert: collName,
    documents: [{_id: 3}],
    txnNumber: NumberLong(txnNumber2),
    stmtId: NumberInt(2),
    autocommit: false
}),
                             ErrorCodes.PreparedTransactionInProgress);

jsTestLog("Aborting the second transaction");

// Make sure we can successfully abort the second transaction after recovery.
assert.commandWorked(sessionDB2.adminCommand(
    {abortTransaction: 1, txnNumber: NumberLong(txnNumber2), autocommit: false}));

jsTestLog("Attempting to run another transction");

// Make sure that we can run another conflicting transaction after recovery without any
// problems.
session.startTransaction();
assert.commandWorked(sessionDB[collName].update({_id: 1}, {_id: 1, a: 3}));
prepareTimestamp = PrepareHelpers.prepareTransaction(session);
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
assert.eq(testDB[collName].findOne({_id: 1}), {_id: 1, a: 3});

replTest.stopSet();
}());
