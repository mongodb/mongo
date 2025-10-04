/**
 * Test that we can successfully reconstruct a prepared transaction that was prepared before the
 * stable timestamp at the end of rollback recovery.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const dbName = "test";
const collName = "rollback_reconstructs_transactions_prepared_before_stable";

const rollbackTest = new RollbackTest(dbName);
let primary = rollbackTest.getPrimary();

// Create collection we're using beforehand.
let testDB = primary.getDB(dbName);
let testColl = testDB.getCollection(collName);
assert.commandWorked(testDB.runCommand({create: collName}));

// Set up another collection for multi-key write in transaction.
const anotherCollName = "anotherColl";
const anotherColl = testDB.getCollection(anotherCollName);
assert.commandWorked(anotherColl.createIndex({"$**": 1}));

// Start a session on the primary.
let session = primary.startSession();
const sessionID = session.getSessionId();
let sessionDB = session.getDatabase(dbName);
let sessionColl = sessionDB.getCollection(collName);
const sessionAnotherColl = sessionDB.getCollection(anotherCollName);

assert.commandWorked(sessionColl.insert({_id: 0}));

// Prepare the transaction on the session.
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 1}));
assert.commandWorked(sessionColl.update({_id: 0}, {$set: {a: 1}}));
// Trigger multi-key writes in the same transaction so that we can also test multi-key writes during
// recovery of the prepared transaction.
assert.commandWorked(sessionAnotherColl.insert({a: [1, 2, 3]}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

// Fastcount reflects the insert of a prepared transaction.
assert.eq(testColl.count(), 2);

// Metrics reflect one inactive prepared transaction.
let metrics = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
assert.eq(1, metrics.transactions.currentPrepared);
assert.eq(1, metrics.transactions.currentInactive);
assert.eq(1, metrics.transactions.currentOpen);

jsTestLog("Do a majority write to advance the stable timestamp past the prepareTimestamp");
// Doing a majority write after preparing the transaction ensures that the stable timestamp is
// past the prepare timestamp because this write must be in the committed snapshot.
assert.commandWorked(testColl.runCommand("insert", {documents: [{_id: 2}]}, {writeConcern: {w: "majority"}}));

// Fastcount reflects the insert of a prepared transaction.
assert.eq(testColl.count(), 3);

// Check that we have one transaction in the transactions table.
assert.eq(primary.getDB("config")["transactions"].find().itcount(), 1);

// The transaction should still be prepared after going through rollback.
rollbackTest.transitionToRollbackOperations();
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations({skipDataConsistencyChecks: true});

// Make sure there is still one transaction in the transactions table. This is because the
// entry in the transactions table is made durable when a transaction is prepared.
assert.eq(primary.getDB("config")["transactions"].find().itcount(), 1);

// Fastcount reflects the insert of the prepared transaction because was put back into prepare
// at the end of rollback.
assert.eq(testColl.count(), 3);

// Metrics still reflect one inactive prepared transaction.
metrics = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
assert.eq(1, metrics.transactions.currentPrepared);
assert.eq(1, metrics.transactions.currentInactive);
assert.eq(1, metrics.transactions.currentOpen);

// Make sure we cannot see the writes from the prepared transaction yet.
assert.sameMembers(testColl.find().toArray(), [{_id: 0}, {_id: 2}]);

// Get the correct primary after the topology changes.
primary = rollbackTest.getPrimary();
testDB = primary.getDB(dbName);
testColl = testDB.getCollection(collName);
rollbackTest.awaitReplication();

// Make sure we can successfully commit the recovered prepared transaction.
session = PrepareHelpers.createSessionWithGivenId(primary, sessionID);
sessionDB = session.getDatabase(dbName);
// The transaction on this session should have a txnNumber of 0. We explicitly set this
// since createSessionWithGivenId does not restore the current txnNumber in the shell.
session.setTxnNumber_forTesting(0);
const txnNumber = session.getTxnNumber_forTesting();

// Make sure we cannot add any operations to a prepared transaction.
assert.commandFailedWithCode(
    sessionDB.runCommand({
        insert: collName,
        txnNumber: NumberLong(txnNumber),
        documents: [{_id: 10}],
        autocommit: false,
    }),
    ErrorCodes.PreparedTransactionInProgress,
);

// Make sure that writing to a document that was updated in the prepared transaction causes
// a write conflict.
assert.commandFailedWithCode(
    sessionDB.runCommand({update: collName, updates: [{q: {_id: 0}, u: {$set: {a: 2}}}], maxTimeMS: 5 * 1000}),
    ErrorCodes.MaxTimeMSExpired,
);

// Commit the transaction.
assert.commandWorked(
    sessionDB.adminCommand({
        commitTransaction: 1,
        commitTimestamp: prepareTimestamp,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
);

// Make sure we can see the effects of the prepared transaction.
assert.sameMembers(testColl.find().toArray(), [{_id: 0, a: 1}, {_id: 1}, {_id: 2}]);
assert.eq(testColl.count(), 3);

rollbackTest.stop();
