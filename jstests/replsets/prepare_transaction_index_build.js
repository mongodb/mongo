/**
 * Tests that background index builds on secondaries block prepared transactions from being
 * prepared.
 * This behavior is necessary because hybrid index builds would otherwise miss prepared transaction
 * writes if the prepared transaction commits after a hybrid index build commits.  The long term
 * solution to this problem is to synchronize index build commits.
 *
 * @tags: [
 *     uses_transactions,
 *     uses_prepare_transaction,
 *     requires_fcv_44,
 * ]
 */
(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

// This test create indexes with fail point enabled on secondary which prevents secondary from
// voting. So, disabling index build commit quorum.
const replTest =
    new ReplSetTest({nodes: 2, nodeOptions: {setParameter: "enableIndexBuildCommitQuorum=false"}});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();

const dbName = "test";
const collName = "prepared_transactions_index_build";
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const bulk = testColl.initializeUnorderedBulkOp();
for (let i = 0; i < 10; ++i) {
    bulk.insert({x: i});
}
assert.commandWorked(bulk.execute());

// activate failpoint to hang index build on secondary.
secondary.getDB("admin").runCommand(
    {configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'});

jsTestLog("Starting a background index build.");
assert.commandWorked(testDB.runCommand({
    createIndexes: collName,
    indexes: [{key: {x: 1}, name: 'x_1'}],
}));

const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

jsTestLog("Starting a transaction that should involve the index and putting it into prepare");

session.startTransaction();
assert.commandWorked(sessionColl.insert({x: 1000}));

const prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: 1});
jsTestLog("Prepared a transaction at " + prepareTimestamp);

jsTestLog("Unblocking index build.");

// finish the index build
secondary.getDB("admin").runCommand(
    {configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'});

// It's illegal to commit a prepared transaction before its prepare oplog entry has been
// majority committed. So wait for prepare oplog entry to be majority committed before issuing
// the commitTransaction command. We know the index build is also done if the prepare has
// finished on the secondary.
jsTestLog(
    "Waiting for prepare oplog entry to be majority committed and all index builds to finish on all nodes.");
PrepareHelpers.awaitMajorityCommitted(replTest, prepareTimestamp);

jsTestLog("Committing txn");
// Commit the transaction.
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
replTest.awaitReplication();

jsTestLog("Testing index integrity");
// Index should work.
assert.eq(
    1000,
    secondary.getDB(dbName).getCollection(collName).find({x: 1000}).hint({x: 1}).toArray()[0].x);
jsTestLog("Shutting down the set");
replTest.stopSet();
}());
