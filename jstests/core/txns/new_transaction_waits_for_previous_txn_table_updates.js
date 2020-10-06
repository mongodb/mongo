/**
 * Tests that a new transaction on a session waits for the previous transaction's transaction table
 * write to be storage-committed before setting the read timestamp of readConcern: snapshot
 * transactions to the all_durable timestamp.
 *
 * This test creates a situation equivalent to the following:
 * 1. Thread 1 prepares txn0 at time 5
 * 2. Thread 2 starts new txn1 that blocks on txn0 since it is on the same session
 * 3. Thread 3 opens oplog hole at time 8 (createCollection)
 * 4. Thread 1 commits txn0 at time 6, but the commit oplog entry (and transaction table update) is
 * written at time 9
 * 5. On thread 2, txn1 should wait until txn0's transaction table update at time 9 becomes durable
 * so that when we open the storage transaction at the all_durable, we will open it at time 9
 * instead of time 7.
 *
 * If we did not wait, the latter would get a write conflict when writing to the txn table because
 * it's reading from time 7 and doesn't see the write from time 9.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, uses_parallel_shell]
 */

(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/parallel_shell_helpers.js");  // for funWithArgs().
load("jstests/libs/fail_point_util.js");

/**
 * Launches a parallel shell to start a new transaction on the session with the given lsid. It
 * performs an insert and then commits. Assumes that there will be an already-prepared transaction
 * on the session, and blocks using a failpoint until the transaction in the parallel shell has
 * begun to block behind the prepared transaction.
 */
function runConcurrentTransactionOnSession(dbName, collName, lsid) {
    let awaitShell;
    // Turn on failpoint that the parallel shell will hit when blocked on prepare.
    const hangTxnFailPoint = configureFailPoint(db, "waitAfterNewStatementBlocksBehindPrepare");

    try {
        function runTransactionOnSession(dbName, collName, lsid) {
            // Use txnNumber : 1 since the active txnNumber will be 0.
            const txnNumber = NumberLong(1);
            // Try to do an insert in a new transaction on the same session.  Note that we're
            // manually including the lsid and stmtId instead of using the session object directly
            // since there's no way to share a session with the parallel shell.
            assert.commandWorked(db.getSiblingDB(dbName).runCommand({
                insert: collName,
                documents: [{x: "blocks_behind_prepare"}],
                readConcern: {level: "snapshot"},
                lsid: lsid,
                txnNumber: txnNumber,
                stmtId: NumberInt(0),
                startTransaction: true,
                autocommit: false
            }));

            assert.commandWorked(db.adminCommand(
                {commitTransaction: 1, lsid: lsid, txnNumber: txnNumber, autocommit: false}));
        }
        // Launch a parallel shell to start a new transaction, insert a document, and commit. These
        // operations should block behind the previous prepared transaction on the session.
        awaitShell =
            startParallelShell(funWithArgs(runTransactionOnSession, dbName, collName, lsid));

        // Wait until parallel shell insert is blocked on prepare.
        hangTxnFailPoint.wait();
    } finally {
        // Disable failpoint to allow the parallel shell to continue - it should still be blocked on
        // prepare. This is needed in a finally block so that if something fails we're guaranteed to
        // turn this failpoint off, so that it doesn't cause problems for subsequent tests.
        hangTxnFailPoint.off();
    }

    return awaitShell;
}

function runConcurrentCollectionCreate(dbName, collName) {
    // Turn on failpoint that the createCollection will hit after reserving an oplog slot.
    // Make sure we specify the collection we are testing on to avoid triggering the failpoint
    // on unrelated createCollection commands that happen to run concurrently.
    const fpData = {nss: dbName + "." + collName};
    const hangCreateFailPoint =
        configureFailPoint(db, "hangAndFailAfterCreateCollectionReservesOpTime", fpData);

    function runCollCreate(dbName, collName) {
        assert.commandFailedWithCode(db.getSiblingDB(dbName).createCollection(collName), 51267);
    }

    // Launch a parallel shell. This thread will hang until we release the failpoint.
    let awaitShell = startParallelShell(funWithArgs(runCollCreate, dbName, collName));

    hangCreateFailPoint.wait();

    return awaitShell;
}

/**
 * Common variables and setup.
 */
const dbName = "test";
const collName = jsTestName();
const testDB = db.getSiblingDB(dbName);

testDB.runCommand({drop: collName});
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const session = testDB.getMongo().startSession();
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb.getCollection(collName);
const lsid = session.getSessionId();

// Start and prepare a transaction, txn0.
session.startTransaction();
assert.commandWorked(sessionColl.insert({y: "prepare_insert"}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

// Launch a concurrent transaction, txn1, which should block behind the active prepared
// transaction.
const awaitTxnShell = runConcurrentTransactionOnSession(dbName, collName, lsid);

// Try to create a collection, which reserves an oplog slot. This oplog slot should be after the
// prepare oplog entry because we have already successfully prepared txn0 and returned a
// prepareTimestamp.
const awaitWriteShell = runConcurrentCollectionCreate(dbName, "newColl");

// Commit the original transaction - this should allow the parallel shell with txn1 to continue
// and start a new transaction.
// Note that we are not using PrepareHelpers.commitTransaction because it calls
// commitTransaction twice, and the second call races with txn1.
assert.commandWorked(session.getDatabase('admin').adminCommand(
    {commitTransaction: 1, commitTimestamp: prepareTimestamp}));

// Release this failpoint so that the createCollection command can finish.
assert.commandWorked(db.adminCommand(
    {configureFailPoint: "hangAndFailAfterCreateCollectionReservesOpTime", mode: "off"}));

// txn1 should be able to commit without getting a WriteConflictError.
awaitTxnShell();

// createCollection command fails with the expected error code, 51267.
awaitWriteShell();

session.endSession();
}());
