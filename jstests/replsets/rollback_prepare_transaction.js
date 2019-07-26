/**
 * Tests that prepared transactions are correctly rolled-back.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/replsets/libs/rollback_test.js");
load("jstests/replsets/libs/rollback_files.js");

const rollbackTest = new RollbackTest();
const rollbackNode = rollbackTest.getPrimary();

const testDB = rollbackNode.getDB("test");
const collName = "rollback_prepare_transaction";
const testColl = testDB.getCollection(collName);

// We perform some operations on the collection aside from starting and preparing a transaction
// in order to cause the count diff computed by replication to be non-zero.
assert.commandWorked(testColl.insert({_id: "a"}));

// Start two separate sessions for running transactions. On 'session1', we will run a prepared
// transaction whose commit operation gets rolled back, and on 'session2', we will run a
// prepared transaction whose prepare operation gets rolled back.
const session1 = rollbackNode.startSession();
const session1DB = session1.getDatabase(testDB.getName());
const session1Coll = session1DB.getCollection(collName);

const session2 = rollbackNode.startSession();
const session2DB = session2.getDatabase(testDB.getName());
const session2Coll = session2DB.getCollection(collName);

// Prepare a transaction whose commit operation will be rolled back.
session1.startTransaction();
assert.commandWorked(session1Coll.insert({_id: "t2_a"}));
assert.commandWorked(session1Coll.insert({_id: "t2_b"}));
assert.commandWorked(session1Coll.insert({_id: "t2_c"}));
let prepareTs = PrepareHelpers.prepareTransaction(session1);

rollbackTest.transitionToRollbackOperations();

// The following operations will be rolled-back.
assert.commandWorked(testColl.insert({_id: "b"}));

session2.startTransaction();
assert.commandWorked(session2Coll.insert({_id: "t1"}));

// Use w: 1 to simulate a prepare that will not become majority-committed.
PrepareHelpers.prepareTransaction(session2, {w: 1});

// Commit the transaction that was prepared before the common point.
PrepareHelpers.commitTransaction(session1, prepareTs);

// This is not exactly correct, but characterizes the current behavior of fastcount, which
// includes the prepared but uncommitted transaction in the collection count.
assert.eq(6, testColl.count());

// Check the visible documents.
arrayEq([{_id: "a"}, {_id: "b"}, {_id: "t2_a"}, {_id: "t2_b"}, {_id: "t2_c"}],
        testColl.find().toArray());

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
// Skip consistency checks so they don't conflict with the prepared transaction.
rollbackTest.transitionToSteadyStateOperations({skipDataConsistencyChecks: true});

// Both the regular insert and prepared insert should be rolled-back.
assert.sameMembers([{_id: "a"}], testColl.find().toArray());

// Confirm that the rollback wrote deleted documents to a file.
const replTest = rollbackTest.getTestFixture();
const expectedDocs = [{_id: "b"}, {_id: "t2_a"}, {_id: "t2_b"}, {_id: "t2_c"}];
checkRollbackFiles(replTest.getDbPath(rollbackNode), testColl.getFullName(), expectedDocs);

// Allow the test to complete by aborting the left over prepared transaction.
jsTestLog("Aborting the prepared transaction on session " + tojson(session1.getSessionId()));
let adminDB = rollbackTest.getPrimary().getDB("admin");
assert.commandWorked(adminDB.adminCommand({
    abortTransaction: 1,
    lsid: session1.getSessionId(),
    txnNumber: session1.getTxnNumber_forTesting(),
    autocommit: false
}));

rollbackTest.stop();
})();
