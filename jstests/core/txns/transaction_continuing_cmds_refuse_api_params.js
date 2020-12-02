/**
 * Tests that passing API parameters into transaction-continuing commands should fail.
 * @tags: [uses_transactions, requires_fcv_47]
 */

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.isMongos().

const errorCode = FixtureHelpers.isMongos(db) ? 4937701 : 4937700;
const commitTxnWithApiVersionErrorCode = FixtureHelpers.isMongos(db) ? 4937702 : 4937700;

const dbName = jsTestName();
const collName = "test";

const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

testColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(
    testDB.runCommand({create: testColl.getName(), writeConcern: {w: "majority"}}));

const session = db.getMongo().startSession();
const sessionAdminDB = session.getDatabase("admin");
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

const doc = {
    x: 1
};

session.startTransaction();

// Verify that the transaction-initiating command is allowed to specify an apiVersion.
assert.commandWorked(sessionColl.runCommand({insert: collName, documents: [doc], apiVersion: "1"}));

// Verify that any transaction-continuing commands cannot specify API parameters.
assert.commandFailedWithCode(
    sessionColl.runCommand({insert: collName, documents: [doc], apiVersion: "1"}), errorCode);
assert.commandFailedWithCode(
    sessionColl.runCommand({insert: collName, documents: [doc], apiVersion: "1", apiStrict: false}),
    errorCode);
assert.commandFailedWithCode(
    sessionColl.runCommand(
        {insert: collName, documents: [doc], apiVersion: "1", apiDeprecationErrors: false}),
    errorCode);
let reply = sessionAdminDB.runCommand({
    commitTransaction: 1,
    txnNumber: session.getTxnNumber_forTesting(),
    autocommit: false,
    apiVersion: "1"
});
assert.commandFailedWithCode(reply, commitTxnWithApiVersionErrorCode);
reply = sessionAdminDB.runCommand({
    abortTransaction: 1,
    txnNumber: session.getTxnNumber_forTesting(),
    autocommit: false,
    apiVersion: "1"
});
assert.commandFailedWithCode(reply, errorCode);

// Transaction-continuing commands without API parameters are allowed.
assert.commandWorked(sessionColl.runCommand({insert: collName, documents: [doc]}));

assert.commandWorked(session.abortTransaction_forTesting());
session.endSession();
})();
