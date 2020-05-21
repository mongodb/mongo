/**
 * Tests that retrying a prepared transaction does not block stepDown.
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

// This test completes with a prepared transaction still active, so we cannot enforce an accurate
// fast count.
TestData.skipEnforceFastCountOnValidate = true;

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();
const dbName = "test";
const collName = "coll";

assert.commandWorked(primary.getDB(dbName).createCollection(collName));

let session = primary.startSession();
let sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

session.startTransaction();

assert.commandWorked(sessionColl.insert([{_id: 1}]));

jsTestLog("Prepare the transaction");
PrepareHelpers.prepareTransaction(session);

jsTestLog("Retry the prepared transaction");
PrepareHelpers.prepareTransaction(session);

// Test that stepDown can proceed.
jsTestLog("Step down primary");
assert.commandWorked(
    primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));

replTest.stopSet();
}());
