/**
 * Test the write conflict behavior between transactional and non-transactional (single document)
 * writes.
 *
 * All writes in MongoDB execute inside transactions. Single document writes (which, until 4.0,
 * categorized all writes), will indefinitely retry, if their associated transaction encounters a
 * WriteConflict error. This differs from the behavior of multi-document transactions, where
 * WriteConflict exceptions that occur inside a transaction are not automatically retried, and are
 * returned to the client. This means that writes to a document D inside a multi-document
 * transaction will effectively "block" any subsequent single document writes to D, until the
 * multi-document transaction commits.
 *
 * Note that in this test we sometimes refer to a single document write as "non-transactional".
 * Internally, single document writes still execute inside a transaction, but we use this
 * terminology to distinguish them from multi-document transactions.
 *
 * @tags: [uses_transactions]
 */

import {WriteConflictHelpers} from "jstests/core/txns/libs/write_conflicts.js";
import {withRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";

const dbName = "test";
const collName = "write_conflicts_with_non_txns";

const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];

// Clean up and create test collection.
testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false
};
const session = db.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];

let initOp;
let txnOp;
let nonTxnOp;
let expectedDocs;

// Performs a single document operation on the test collection. Returns the command result object.
function singleDocWrite(dbName, collName, op) {
    const testColl = db.getSiblingDB(dbName)[collName];
    return testColl.runCommand(op);
}

// Returns true if a single document operation has started running on the server.
function writeStarted(opType) {
    return testDB.currentOp().inprog.some(op => {
        return op.active && (op.ns === testColl.getFullName()) && (op.op === opType) &&
            (op.writeConflicts > 0);
    });
}

// Returns true if the number of write conflicts after is at least 1 more than the number of write
// conflicts before. When exact is used, it returns true if after is exactly 1 more than before.
function validateWriteConflictsBeforeAndAfter(before, after, exact = false) {
    if (before != null && after != null) {
        // Transactions on sharded collections can land on multiple shards and increment the
        // total WCE metric by the number of shards involved. Similarly, BulkWriteOverride turns
        // a single op into multiple writes and causes multiple WCEs.
        if (FixtureHelpers.isSharded(testColl) || TestData.runningWithBulkWriteOverride || !exact) {
            assert.gte(after, before + 1);
        } else {
            assert.eq(after, before + 1);
        }
    }
}

/**
 * A non-transactional (single document) write should keep retrying when attempting to perform the
 * write operation that conflicts with a previous write done by a running transaction, and should be
 * allowed to continue and complete successfully after the transaction aborts. Since
 * non-transactional writes are retried multiple times and each one of those retries count as a
 * write conflict, the only guarantee is that the number of writes conflicts after is at least 1
 * more than the number of writes conflicts before the execution of the write.
 */
function TWriteFirst(txnOp, nonTxnOp, nonTxnOpType, expectedDocs, initOp) {
    withRetryOnTransientTxnError(
        () => {
            // Make sure the collection is empty.
            assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));

            // Initialize the collection state.
            if (initOp !== undefined) {
                assert.commandWorked(testColl.runCommand(initOp));
            }

            jsTestLog("Start a multi-document transaction.");
            session.startTransaction();
            assert.commandWorked(sessionColl.runCommand(txnOp));

            jsTestLog("Doing conflicting single document write in separate thread.");
            const writeConflictsBefore =
                WriteConflictHelpers.getWriteConflictsFromAllShards(testColl);
            let thread = new Thread(singleDocWrite, dbName, collName, nonTxnOp);
            thread.start();

            // Wait for the single doc write to start.
            assert.soon(() => writeStarted(nonTxnOpType), "NonTxnOp not started");

            // Abort the transaction, which should allow the single document write to finish and
            // insert its document successfully.
            jsTestLog("Abort the multi-document transaction.");
            assert.commandWorked(session.abortTransaction_forTesting());
            thread.join();
            assert.commandWorked(thread.returnData());

            // Validate that a write conflict was detected
            const writeConflictsAfter =
                WriteConflictHelpers.getWriteConflictsFromAllShards(testColl);
            validateWriteConflictsBeforeAndAfter(writeConflictsBefore, writeConflictsAfter);
        },
        () => {
            session.abortTransaction_forTesting();
        });

    // Check the final documents.
    assert.sameMembers(expectedDocs, testColl.find().toArray());

    // Clean up the test collection.
    assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));
}

/**
 * A transaction that tries to write to a document that was updated by a non-transaction after
 * it started should fail with a WriteConflict. Since it is not retried, it is guarantee that the
 * number of writes conflicts after will be exactly 1 more than the number of writes conflicts
 * before.
 */
function TWriteSecond(txnOp, nonTxnOp, expectedDocs, initOp) {
    withRetryOnTransientTxnError(
        () => {
            // Make sure the collection is empty.
            assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));

            // Initialize the collection state.
            if (initOp !== undefined) {
                assert.commandWorked(testColl.runCommand(initOp));
            }

            jsTestLog("Start a multi-document transaction.");
            session.startTransaction();
            assert.commandWorked(sessionColl.runCommand({find: collName}));

            jsTestLog("Do a single document Op outside of the transaction.");
            assert.commandWorked(testColl.runCommand(nonTxnOp));

            jsTestLog("Executing a conflicting document Op inside the multi-document transaction.");
            const writeConflictsBefore =
                WriteConflictHelpers.getWriteConflictsFromAllShards(testColl);
            assert.commandFailedWithCode(sessionColl.runCommand(txnOp), ErrorCodes.WriteConflict);
            assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                         ErrorCodes.NoSuchTransaction);
            const writeConflictsAfter =
                WriteConflictHelpers.getWriteConflictsFromAllShards(testColl);
            validateWriteConflictsBeforeAndAfter(
                writeConflictsBefore, writeConflictsAfter, true /*exact*/);
        },
        () => {
            session.abortTransaction_forTesting();
        });

    // Check the final documents.
    assert.sameMembers(expectedDocs, testColl.find().toArray());

    // Clean up the test collection.
    assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));
}

jsTestLog("insert-insert conflict.");
// Two conflicting documents to be inserted by a multi-document transaction and a
// non-transactional write, respectively.
txnOp = {
    insert: collName,
    documents: [{_id: 1}]
};
nonTxnOp = {
    insert: collName,
    documents: [{_id: 1, nonTxn: true}]
};
expectedDocs = [{_id: 1, nonTxn: true}];
TWriteFirst(txnOp, nonTxnOp, "insert", expectedDocs);
TWriteSecond(txnOp, nonTxnOp, expectedDocs);

jsTestLog("update-update conflict.");
initOp = {
    insert: collName,
    documents: [{_id: 1}]
};  // the document to update.
txnOp = {
    update: collName,
    updates: [{q: {_id: 1}, u: {$set: {t1: 1}}}]
};
nonTxnOp = {
    update: collName,
    updates: [{q: {_id: 1}, u: {$set: {t2: 1}}}]
};
expectedDocs = [{_id: 1, t2: 1}];
TWriteFirst(txnOp, nonTxnOp, "update", expectedDocs, initOp);
TWriteSecond(txnOp, nonTxnOp, expectedDocs, initOp);

jsTestLog("upsert-upsert conflict");
txnOp = {
    update: collName,
    updates: [{q: {_id: 1}, u: {$set: {t1: 1}}, upsert: true}]
};
nonTxnOp = {
    update: collName,
    updates: [{q: {_id: 1}, u: {$set: {t2: 1}}, upsert: true}]
};
expectedDocs = [{_id: 1, t2: 1}];
TWriteFirst(txnOp, nonTxnOp, "update", expectedDocs);
TWriteSecond(txnOp, nonTxnOp, expectedDocs);

jsTestLog("delete-delete conflict");
initOp = {
    insert: collName,
    documents: [{_id: 1}]
};  // the document to delete.
txnOp = {
    delete: collName,
    deletes: [{q: {_id: 1}, limit: 1}]
};
nonTxnOp = {
    delete: collName,
    deletes: [{q: {_id: 1}, limit: 1}]
};
expectedDocs = [];
TWriteFirst(txnOp, nonTxnOp, "remove", expectedDocs, initOp);
TWriteSecond(txnOp, nonTxnOp, expectedDocs, initOp);

jsTestLog("update-delete conflict");
initOp = {
    insert: collName,
    documents: [{_id: 1}]
};  // the document to delete/update.
txnOp = {
    update: collName,
    updates: [{q: {_id: 1}, u: {$set: {t1: 1}}}]
};
nonTxnOp = {
    delete: collName,
    deletes: [{q: {_id: 1}, limit: 1}]
};
expectedDocs = [];
TWriteFirst(txnOp, nonTxnOp, "remove", expectedDocs, initOp);
TWriteSecond(txnOp, nonTxnOp, expectedDocs, initOp);

jsTestLog("delete-update conflict");
initOp = {
    insert: collName,
    documents: [{_id: 1}]
};  // the document to delete/update.
txnOp = {
    delete: collName,
    deletes: [{q: {_id: 1}, limit: 1}]
};
nonTxnOp = {
    update: collName,
    updates: [{q: {_id: 1}, u: {$set: {t2: 1}}}]
};
expectedDocs = [{_id: 1, t2: 1}];
TWriteFirst(txnOp, nonTxnOp, "update", expectedDocs, initOp);
TWriteSecond(txnOp, nonTxnOp, expectedDocs, initOp);
