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

(function() {

    "use strict";

    load('jstests/libs/parallelTester.js');  // for ScopedThread.

    const dbName = "test";
    const collName = "write_conflicts_with_non_txns";

    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB[collName];

    // Clean up and create test collection.
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const sessionOptions = {causalConsistency: false};
    const session = db.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    // Two conflicting documents to be inserted by a multi-document transaction and a
    // non-transactional write, respectively.
    const txnDoc = {_id: 1};
    const nonTxnDoc = {_id: 1, nonTxn: true};

    // Performs a single document insert on the test collection. Returns the command result object.
    function singleDocWrite(dbName, collName, doc) {
        const testColl = db.getSiblingDB(dbName)[collName];
        return testColl.runCommand({insert: collName, documents: [doc]});
    }

    // Returns true if a single document insert has started running on the server.
    function writeStarted() {
        return testDB.currentOp().inprog.some(op => {
            return op.active && (op.ns === testColl.getFullName()) && (op.op === "insert");
        });
    }

    /**
     * A non-transactional (single document) write should block when trying to insert a document
     * that conflicts with a previous write done by a running transaction, and should be allowed to
     * continue after the transaction commits. If 'maxTimeMS' is specified, a single document write
     * should timeout after the given time limit if there is a write conflict.
     */

    jsTestLog("Start a multi-document transaction with a document insert.");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert(txnDoc));

    jsTestLog("Do a conflicting single document insert outside of transaction with maxTimeMS.");
    assert.commandFailedWithCode(
        testColl.runCommand({insert: collName, documents: [nonTxnDoc], maxTimeMS: 100}),
        ErrorCodes.ExceededTimeLimit);

    jsTestLog("Doing conflicting single document write in separate thread.");
    let thread = new ScopedThread(singleDocWrite, dbName, collName, nonTxnDoc);
    thread.start();

    // Wait for the single doc write to start.
    assert.soon(writeStarted);

    // Commit the transaction, which should allow the single document write to finish. Since the
    // single doc write should get serialized after the transaction, we expect it to fail with a
    // duplicate key error.
    jsTestLog("Commit the multi-document transaction.");
    session.commitTransaction();
    thread.join();
    assert.commandFailedWithCode(thread.returnData(), ErrorCodes.DuplicateKey);

    // Check the final documents.
    assert.docEq([txnDoc], testColl.find().toArray());

    // Clean up the test collection.
    assert.commandWorked(testColl.remove({}));

    /**
     * A non-transactional (single document) write should block when trying to insert a document
     * that conflicts with a previous write done by a running transaction, and should be allowed to
     * continue and complete successfully after the transaction aborts.
     */

    jsTestLog("Start a multi-document transaction with a document insert.");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert(txnDoc));

    jsTestLog("Doing conflicting single document write in separate thread.");
    thread = new ScopedThread(singleDocWrite, dbName, collName, nonTxnDoc);
    thread.start();

    // Wait for the single doc write to start.
    assert.soon(writeStarted);

    // Abort the transaction, which should allow the single document write to finish and insert its
    // document successfully.
    jsTestLog("Abort the multi-document transaction.");
    session.abortTransaction();
    thread.join();
    assert.commandWorked(thread.returnData());

    // Check the final documents.
    assert.docEq([nonTxnDoc], testColl.find().toArray());

    // Clean up the test collection.
    assert.commandWorked(testColl.remove({}));

    /**
     * A transaction that tries to write to a document that was updated by a non-transaction after
     * it started should fail with a WriteConflict.
     */

    jsTestLog("Start a multi-document transaction.");
    session.startTransaction();
    assert.commandWorked(sessionColl.runCommand({find: collName}));

    jsTestLog("Do a single document insert outside of the transaction.");
    assert.commandWorked(testColl.insert(nonTxnDoc));

    jsTestLog("Insert a conflicting document inside the multi-document transaction.");
    assert.commandFailedWithCode(sessionColl.insert(txnDoc), ErrorCodes.WriteConflict);
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // Check the final documents.
    assert.docEq([nonTxnDoc], testColl.find().toArray());

}());