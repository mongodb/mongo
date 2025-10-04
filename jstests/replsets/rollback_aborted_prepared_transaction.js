/**
 * Test that rollback can successfully roll back an aborted prepared transaction.
 * Also test that it is impossible to commit a prepared transaction whose prepare oplog entry has
 * not yet been majority committed. This case should never happen when prepareTransaction is driven
 * by the transaction coordinator because transaction coordinator should always call
 * prepareTransaction with {w : "majority"}.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const dbName = "test";
const collName = "rollback_aborted_prepared_transaction";

const rollbackTest = new RollbackTest(dbName);
let primary = rollbackTest.getPrimary();

// Create collection we're using beforehand.
let testDB = primary.getDB(dbName);
let testColl = testDB.getCollection(collName);

testDB.runCommand({drop: collName});
assert.commandWorked(testDB.runCommand({create: collName}));
assert.commandWorked(testColl.insert({_id: 0}));

// Start two sessions on the primary.
let session = primary.startSession();
const sessionID = session.getSessionId();
let sessionDB = session.getDatabase(dbName);
let sessionColl = sessionDB.getCollection(collName);

let session2 = primary.startSession();
let sessionColl2 = session2.getDatabase(dbName).getCollection(collName);

// The following transaction will be rolled back.
rollbackTest.transitionToRollbackOperations();

// Prepare the transaction on the session.
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 1}));
PrepareHelpers.prepareTransaction(session, {w: 1});

assert.eq(testColl.find().itcount(), 1);
// This characterizes the current fastcount behavior, which is that active prepared transactions
// contribute to the fastcount.
assert.eq(testColl.count(), 2);

// Abort the transaction explicitly.
assert.commandWorked(session.abortTransaction_forTesting());

assert.eq(testColl.find().itcount(), 1);
assert.eq(testColl.count(), 1);

// Test that it is impossible to commit a prepared transaction whose prepare oplog entry has not
// yet majority committed. This also aborts the transaction.
session2.startTransaction();
assert.commandWorked(sessionColl2.insert({_id: 2}));
let prepareTimestamp = PrepareHelpers.prepareTransaction(session2, {w: 1});
let res = assert.commandFailedWithCode(
    PrepareHelpers.commitTransaction(session2, prepareTimestamp),
    ErrorCodes.InvalidOptions,
);
assert(res.errmsg.includes("cannot be run before its prepare oplog entry has been majority committed"), res);
assert.eq(testColl.find().itcount(), 1);
assert.eq(testColl.count(), 1);

// Check that we have two transactions in the transactions table.
assert.eq(primary.getDB("config")["transactions"].find().itcount(), 2);

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Make sure there are no transactions in the transactions table. This is because both the abort
// and prepare operations are rolled back, and the entry in the transactions table is only made
// durable when a transaction is prepared.
assert.eq(primary.getDB("config")["transactions"].find().itcount(), 0);

// Make sure the first collection only has one document since the prepared insert was rolled
// back.
assert.eq(sessionColl.find().itcount(), 1);
assert.eq(sessionColl.count(), 1);

// Get the new primary after the topology changes.
primary = rollbackTest.getPrimary();
testDB = primary.getDB(dbName);
testColl = testDB.getCollection(collName);

// Make sure we can successfully run a prepared transaction on the same session after going
// through rollback.
session = PrepareHelpers.createSessionWithGivenId(primary, sessionID);
sessionDB = session.getDatabase(dbName);
sessionColl = sessionDB.getCollection(collName);

session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 1}));
prepareTimestamp = PrepareHelpers.prepareTransaction(session);
PrepareHelpers.commitTransaction(session, prepareTimestamp);

assert.eq(testColl.find().itcount(), 2);
assert.eq(testColl.count(), 2);

rollbackTest.stop();
