/**
 * Test that we can't call prepareTransaction if there isn't an active transaction on the session.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

const dbName = "test";
const collName = "ensure_active_txn_for_prepare_transaction";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const session = db.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

jsTestLog("Test that we can't call prepareTransaction if there was never a transaction on " +
          "the session");
assert.commandFailedWithCode(
    sessionDB.adminCommand(
        {prepareTransaction: 1, txnNumber: NumberLong(0), stmtId: NumberInt(1), autocommit: false}),
    ErrorCodes.NoSuchTransaction);

jsTestLog("Test that we can't call prepareTransaction if the most recent transaction was aborted");
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 1}));
assert.commandWorked(session.abortTransaction_forTesting());

assert.commandFailedWithCode(
    sessionDB.adminCommand(
        {prepareTransaction: 1, txnNumber: NumberLong(0), stmtId: NumberInt(1), autocommit: false}),
    ErrorCodes.NoSuchTransaction);

jsTestLog(
    "Test that we can't call prepareTransaction if the most recent transaction was committed");
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 1}));
assert.commandWorked(session.commitTransaction_forTesting());

assert.commandFailedWithCode(
    sessionDB.adminCommand(
        {prepareTransaction: 1, txnNumber: NumberLong(1), stmtId: NumberInt(1), autocommit: false}),
    ErrorCodes.TransactionCommitted);

session.endSession();
}());
