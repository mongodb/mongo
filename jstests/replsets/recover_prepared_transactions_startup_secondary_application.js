/**
 * Test that startup recovery can recover multiple prepared transactions and that the node starting
 * up can then successfully apply commit or abort transaction oplog entries during secondary oplog
 * application.
 *
 * @tags: [requires_persistence, uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

const replTest = new ReplSetTest({nodes: 2});
const nodes = replTest.startSet();

// Increase the election timeout to 24 hours so that we do not accidentally trigger an election
// while the secondary is restarting.
replTest.initiateWithHighElectionTimeout();

const primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

const dbName = "test";
const collName = "recover_prepared_transactions_startup_secondary_application";
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testDB.runCommand({create: collName}));

const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

const session2 = primary.startSession();
const sessionDB2 = session2.getDatabase(dbName);
const sessionColl2 = sessionDB2.getCollection(collName);

assert.commandWorked(sessionColl.insert({_id: 1}));
assert.commandWorked(sessionColl2.insert({_id: 2}));

replTest.awaitReplication();

jsTestLog("Disable snapshotting on all nodes");

// Disable snapshotting on all members of the replica set so that further operations do not
// enter the majority snapshot.
nodes.forEach(node => assert.commandWorked(node.adminCommand(
                  {configureFailPoint: "disableSnapshotting", mode: "alwaysOn"})));

session.startTransaction();
assert.commandWorked(sessionColl.update({_id: 1}, {_id: 1, a: 1}));
let prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: 1});
jsTestLog("Prepared a transaction at " + prepareTimestamp);

session2.startTransaction();
assert.commandWorked(sessionColl2.update({_id: 2}, {_id: 2, a: 1}));
const prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2, {w: 1});
jsTestLog("Prepared another transaction at " + prepareTimestamp2);

const lsid = session.getSessionId();
const txnNumber = session.getTxnNumber_forTesting();

const lsid2 = session2.getSessionId();
const txnNumber2 = session2.getTxnNumber_forTesting();

jsTestLog("Restarting node");

// Perform a clean shutdown and restart. Note that the 'disableSnapshotting' failpoint will be
// unset on the node following the restart.
replTest.stop(secondary, undefined, {skipValidation: true});
secondary = replTest.start(secondary, {}, true);

jsTestLog("Secondary was restarted");

assert.commandWorked(
    primary.adminCommand({configureFailPoint: "disableSnapshotting", mode: "off"}));

// It's illegal to commit a prepared transaction before its prepare oplog entry has been
// majority committed. So wait for prepare oplog entry to be majority committed before issuing
// the commitTransaction command.
PrepareHelpers.awaitMajorityCommitted(replTest, prepareTimestamp2);

// Wait for the node to complete recovery before trying to read from it.
replTest.awaitSecondaryNodes();
secondary.setSlaveOk();

jsTestLog("Checking that the first transaction is properly prepared");

// Make sure that we can't read changes to the document from either transaction after recovery.
const secondaryTestColl = secondary.getDB(dbName).getCollection(collName);
assert.eq(secondaryTestColl.find({_id: 1}).toArray(), [{_id: 1}]);
assert.eq(secondaryTestColl.find({_id: 2}).toArray(), [{_id: 2}]);

jsTestLog("Committing the first transaction");

// Make sure we can successfully commit the first transaction after recovery and that we can see
// all its changes when we read from the secondary.
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
replTest.awaitReplication();
assert.eq(secondaryTestColl.find().sort({_id: 1}).toArray(), [{_id: 1, a: 1}, {_id: 2}]);

jsTestLog("Aborting the second transaction");

// Make sure we can successfully abort the second transaction after recovery and that we can't
// see any of its operations when we read from the secondary.
assert.commandWorked(session2.abortTransaction_forTesting());
replTest.awaitReplication();
assert.eq(secondaryTestColl.find().sort({_id: 1}).toArray(), [{_id: 1, a: 1}, {_id: 2}]);

jsTestLog("Attempting to run another transction");

// Make sure that we can run another conflicting transaction after recovery without any
// problems and that we can see its changes when we read from the secondary.
session.startTransaction();
assert.commandWorked(sessionDB[collName].update({_id: 1}, {_id: 1, a: 3}));
prepareTimestamp = PrepareHelpers.prepareTransaction(session);
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
assert.eq(testColl.findOne({_id: 1}), {_id: 1, a: 3});
replTest.awaitReplication();
assert.eq(secondaryTestColl.findOne({_id: 1}), {_id: 1, a: 3});

replTest.stopSet();
}());
