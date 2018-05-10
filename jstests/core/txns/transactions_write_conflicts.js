/**
 * Verify that transactions correctly abort for all possible types of write conflicts.
 *
 * BACKGROUND:
 *
 * Snapshot isolation guarantees that every transaction, conceptually, runs against its own,
 * consistent "snapshot" of the database, which reflects the effects of all previously committed
 * transactions. Every transaction performs its reads and writes against this snapshot, adjusting
 * for the effects of any writes executed inside the transaction. Snapshot isolation enforces a
 * policy for serializing writes between transactions known as the "First Updater Wins" rule.
 * Intuitively, this rule states that concurrent transactions are not allowed to write to the same
 * document. It enforces this via an optimistic concurrency control mechanism, as opposed to a
 * pessimistic one i.e. locking the document. If two concurrent transactions T1 and T2 write to the
 * same document, only the first writer will succeed. So, for example, if transaction T2 tries to
 * write to a document already written to by T1 this will cause T2's write to fail with a
 * "WriteConflict" error, and transaction T2 will be aborted.
 *
 * In MongoDB, a "write" by a transaction is any insert, update, or delete to a document. This means
 * that all of these operation types can produce write conflicts with each other as a result of
 * running in concurrent transactions. The test cases below illustrate the possible write conflict
 * types for both single and multi-document writes.
 *
 * Note that two transactions can be "concurrent" even if they don't necessarily execute on
 * different threads within the server. Transactions under snapshot isolation are defined to be
 * "concurrent" if their transactional lifetimes overlap. The lifetime of a transaction is the
 * interval [Start(T), Commit(T)], where Start(T) is the read timestamp of transaction (the
 * timestamp of the snapshot it executes against) and Commit(T) is the timestamp of the
 * transaction's commit operation. In the tests below, we use two separate sessions so we can
 * arbitrarily interleave operations between two different transactions, simulating the execution of
 * two "truly" concurrent transactions.
 *
 *  @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "transactions_write_conflicts";

    const testDB = db.getSiblingDB(dbName);
    const coll = testDB[collName];

    // Clean up and create test collection.
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    testDB.runCommand({create: coll.getName(), writeConcern: {w: "majority"}});

    // Initiate two sessions.
    const sessionOptions = {causalConsistency: false};
    const session1 = testDB.getMongo().startSession(sessionOptions);
    const session1Coll = session1.getDatabase(dbName)[collName];
    const session2 = testDB.getMongo().startSession(sessionOptions);
    const session2Coll = session2.getDatabase(dbName)[collName];

    /**
     * Test that two given transaction operations produce a write conflict correctly.
     *
     * Creates two transactions, T1 and T2. Executes 'txn1Op' in T1 and then 'txn2Op' in T2. Always
     * expects 'txn2Op' to fail with a write conflict. After executing each transaction operation,
     * tries to commit both transactions, expecting T1 to commit successfully and T2 to fail. Then
     * it will check the expected state of the collection, and finally remove all documents from the
     * test collection.
     *
     * @param txn1Op - the command object to execute on transaction 1.
     * @param txn2Op - the command object to execute on transaction 2.
     * @param expectedDocs - an array of documents that is the expected state of the test collection
     * after both transactions are committed/aborted.
     * @param initOp (optional) - an operation to execute before starting either transaction.
     */
    function writeConflictTest(txn1Op, txn2Op, expectedDocs, initOp) {
        if (initOp !== undefined) {
            assert.commandWorked(coll.runCommand(initOp));
        }

        jsTestLog("Executing write conflict test. \n transaction 1 op: " + tojson(txn1Op) +
                  "\n transaction 2 op: " + tojson(txn2Op));

        session1.startTransaction();
        session2.startTransaction();

        assert.commandWorked(session1Coll.runCommand(txn1Op));
        assert.commandFailedWithCode(session2Coll.runCommand(txn2Op), ErrorCodes.WriteConflict);

        session1.commitTransaction();
        assert.commandFailedWithCode(session2.commitTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);

        // Check the final state of the collection.
        assert.docEq(expectedDocs, coll.find().toArray());

        // Clean up the collection.
        coll.remove({}, {writeConcern: {w: "majority"}});
    }

    /***********************************************************************************************
     * Single document write conflicts.
     **********************************************************************************************/

    jsTestLog("Test single document write conflicts.");

    print("insert-insert conflict.");
    let t1Op = {insert: collName, documents: [{_id: 1, t1: 1}]};
    let t2Op = {insert: collName, documents: [{_id: 1, t2: 1}]};
    let expectedDocs = [{_id: 1, t1: 1}];
    writeConflictTest(t1Op, t2Op, expectedDocs);

    print("update-update conflict");
    let initOp = {insert: collName, documents: [{_id: 1}]};  // the document to update.
    t1Op = {update: collName, updates: [{q: {_id: 1}, u: {$set: {t1: 1}}}]};
    t2Op = {update: collName, updates: [{q: {_id: 1}, u: {$set: {t2: 1}}}]};
    expectedDocs = [{_id: 1, t1: 1}];
    writeConflictTest(t1Op, t2Op, expectedDocs, initOp);

    print("upsert-upsert conflict");
    t1Op = {update: collName, updates: [{q: {_id: 1}, u: {$set: {t1: 1}}, upsert: true}]};
    t2Op = {update: collName, updates: [{q: {_id: 1}, u: {$set: {t2: 1}}, upsert: true}]};
    expectedDocs = [{_id: 1, t1: 1}];
    writeConflictTest(t1Op, t2Op, expectedDocs, initOp);

    print("delete-delete conflict");
    initOp = {insert: collName, documents: [{_id: 1}]};  // the document to delete.
    t1Op = {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]};
    t2Op = {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]};
    expectedDocs = [];
    writeConflictTest(t1Op, t2Op, expectedDocs, initOp);

    print("update-delete conflict");
    initOp = {insert: collName, documents: [{_id: 1}]};  // the document to delete/update.
    t1Op = {update: collName, updates: [{q: {_id: 1}, u: {$set: {t1: 1}}}]};
    t2Op = {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]};
    expectedDocs = [{_id: 1, t1: 1}];
    writeConflictTest(t1Op, t2Op, expectedDocs, initOp);

    print("delete-update conflict");
    initOp = {insert: collName, documents: [{_id: 1}]};  // the document to delete/update.
    t1Op = {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]};
    t2Op = {update: collName, updates: [{q: {_id: 1}, u: {$set: {t2: 1}}}]};
    expectedDocs = [];
    writeConflictTest(t1Op, t2Op, expectedDocs, initOp);

    /***********************************************************************************************
     * Multi-document and predicate based write conflicts.
     **********************************************************************************************/

    jsTestLog("Test multi-document and predicate based write conflicts.");

    print("batch insert-batch insert conflict");
    t1Op = {insert: collName, documents: [{_id: 1}, {_id: 2}, {_id: 3}]};
    t2Op = {insert: collName, documents: [{_id: 2}, {_id: 3}, {_id: 4}]};
    expectedDocs = [{_id: 1}, {_id: 2}, {_id: 3}];
    writeConflictTest(t1Op, t2Op, expectedDocs);

    print("multiupdate-multiupdate conflict");
    initOp = {
        insert: collName,
        documents: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]  // the documents to update/delete.
    };
    // Predicate intersection: [{_id: 2}, {_id: 3}]
    t1Op = {update: collName, updates: [{q: {_id: {$lte: 3}}, u: {$set: {t1: 1}}, multi: true}]};
    t2Op = {update: collName, updates: [{q: {_id: {$gte: 2}}, u: {$set: {t2: 2}}, multi: true}]};
    expectedDocs = [{_id: 1, t1: 1}, {_id: 2, t1: 1}, {_id: 3, t1: 1}, {_id: 4}];
    writeConflictTest(t1Op, t2Op, expectedDocs, initOp);

    print("multiupdate-multidelete conflict");
    initOp = {
        insert: collName,
        documents: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]  // the documents to update/delete.
    };
    // Predicate intersection: [{_id: 2}, {_id: 3}]
    t1Op = {update: collName, updates: [{q: {_id: {$lte: 3}}, u: {$set: {t1: 1}}, multi: true}]};
    t2Op = {delete: collName, deletes: [{q: {_id: {$gte: 2}}, limit: 0}]};
    expectedDocs = [{_id: 1, t1: 1}, {_id: 2, t1: 1}, {_id: 3, t1: 1}, {_id: 4}];
    writeConflictTest(t1Op, t2Op, expectedDocs, initOp);

    print("multidelete-multiupdate conflict");
    initOp = {
        insert: collName,
        documents: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]  // the documents to update/delete.
    };
    // Predicate intersection: [{_id: 2}, {_id: 3}]
    t1Op = {delete: collName, deletes: [{q: {_id: {$lte: 3}}, limit: 0}]};
    t2Op = {update: collName, updates: [{q: {_id: {$gte: 2}}, u: {$set: {x2: 1}}, multi: true}]};
    expectedDocs = [{_id: 4}];
    writeConflictTest(t1Op, t2Op, expectedDocs, initOp);

    print("multidelete-multidelete conflict");
    initOp = {
        insert: collName,
        documents: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]  // the documents to delete.
    };
    // Predicate intersection: [{_id: 2}, {_id: 3}]
    t1Op = {delete: collName, deletes: [{q: {_id: {$lte: 3}}, limit: 0}]};
    t2Op = {delete: collName, deletes: [{q: {_id: {$gte: 2}}, limit: 0}]};
    expectedDocs = [{_id: 4}];
    writeConflictTest(t1Op, t2Op, expectedDocs, initOp);

    session1.endSession();
    session2.endSession();
}());
