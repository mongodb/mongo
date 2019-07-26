/**
 * Tests that a server can still be shut down while it has prepared transactions pending.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const conn = replTest.getPrimary();

const dbName = "test";
const collName = "shutdown_with_prepared_txn";
const testDB = conn.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const session = conn.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

jsTestLog("Starting a simple transaction and putting it into prepare");

session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 1}));

PrepareHelpers.prepareTransaction(session);

jsTestLog("Shutting down the set with the transaction still in prepare state");
// Skip validation during ReplSetTest cleanup since validate() will block behind the prepared
// transaction's locks when trying to take a collection X lock.
replTest.stopSet(null /*signal*/, false /*forRestart*/, {skipValidation: true});
}());
