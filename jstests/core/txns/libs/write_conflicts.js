/**
 * Helper functions for testing write conflicts between concurrent, multi-document transactions.
 *
 * @tags: [uses_transactions]
 *
 */
var WriteConflictHelpers = (function() {

    /**
     * Write conflict test cases.
     *
     * Each test case starts two transactions, T1 and T2, and runs the two given operations,
     * 'txn1Op' and 'txn2Op' in each respective transaction in an ordering defined by that
     * particular test case. A write conflict test case function expects the given operations to
     * produce a write conflict if executed in separate, concurrent transactions, given the initial
     * state of the test collection. The initial state can be can be specified with 'initOp', which
     * will be executed before either transaction is started. Each test case expects that the test
     * collection is empty before the test case is executed.
     *
     * Transaction events:
     *
     * c - commit
     * a - abort due to write conflict
     * w - conflicting write operation
     *
     */

    /**
     * Write conflict test case, ordering 1.
     *
     * Transactional lifetimes:
     *
     * T1: |-------w------c
     * T2:    |--------a
     *
     */
    function T1StartsFirstAndWins(coll, session1, session2, txn1Op, txn2Op, initOp) {
        // Initialize the collection state.
        if (initOp !== undefined) {
            assert.commandWorked(coll.runCommand(initOp));
        }

        let collName = coll.getName();
        const session1Coll = session1.getDatabase(coll.getDB().getName())[collName];
        const session2Coll = session2.getDatabase(coll.getDB().getName())[collName];

        session1.startTransaction();
        session2.startTransaction();

        assert.commandWorked(session1Coll.runCommand(txn1Op));
        assert.commandFailedWithCode(session2Coll.runCommand(txn2Op), ErrorCodes.WriteConflict);

        session1.commitTransaction();
        assert.commandFailedWithCode(session2.commitTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
    }

    /**
     * Write conflict test case, ordering 2.
     *
     * Transactional lifetimes:
     *
     * T1: |--------------a
     * T2:    |---w---c
     *
     */
    function T2StartsSecondAndWins(coll, session1, session2, txn1Op, txn2Op, initOp) {
        // Initialize the collection state.
        if (initOp !== undefined) {
            assert.commandWorked(coll.runCommand(initOp));
        }

        let collName = coll.getName();
        const session1Coll = session1.getDatabase(coll.getDB().getName())[collName];
        const session2Coll = session2.getDatabase(coll.getDB().getName())[collName];

        session1.startTransaction();
        session2.startTransaction();

        assert.commandWorked(session1Coll.runCommand({find: collName}));  // Start T1 with a no-op.
        assert.commandWorked(session2Coll.runCommand(txn2Op));
        session2.commitTransaction();

        assert.commandFailedWithCode(session1Coll.runCommand(txn1Op), ErrorCodes.WriteConflict);
        assert.commandFailedWithCode(session1.commitTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
    }

    /**
     * Runs a specific write conflict test case.
     *
     * See the documentation above for further explanation of these test cases. After running the
     * test case, this function checks the final expected state of the collection, and then removes
     * all documents from the test collection.
     *
     * @param coll - a collection object that represents which collection to execute the test
     * operations against.
     * @param txn1Op - the command object to execute on transaction 1.
     * @param txn2Op - the command object to execute on transaction 2.
     * @param expectedDocs - an array of documents that is the expected state of the test collection
     * after both transactions are committed/aborted.
     * @param writeConflictTestCase - the write conflict test case to execute. it should be one of
     * the test case functions defined above in 'WriteConflictsTestCases'.
     * @param initOp (optional) - an operation to execute against the test collection before
     * starting either transaction.
     */
    function writeConflictTest(coll, txn1Op, txn2Op, expectedDocs, writeConflictTestCase, initOp) {
        // Make sure the collection is empty.
        assert.commandWorked(coll.remove({}, {writeConcern: {w: "majority"}}));

        const conn = coll.getDB().getMongo();

        // Initiate two sessions.
        const sessionOptions = {causalConsistency: false};
        const session1 = conn.startSession(sessionOptions);
        const session2 = conn.startSession(sessionOptions);

        jsTestLog("Executing write conflict test, case '" + writeConflictTestCase.name +
                  "'. \n transaction 1 op: " + tojson(txn1Op) + "\n transaction 2 op: " +
                  tojson(txn2Op));

        // Run the specified write conflict test.
        writeConflictTestCase(coll, session1, session2, txn1Op, txn2Op, initOp);

        // Check the final state of the collection.
        assert.docEq(expectedDocs, coll.find().toArray());

        // Clean up the collection.
        assert.commandWorked(coll.remove({}, {writeConcern: {w: "majority"}}));
    }

    return {
        writeConflictTest: writeConflictTest,
        T1StartsFirstAndWins: T1StartsFirstAndWins,
        T2StartsSecondAndWins: T2StartsSecondAndWins
    };
})();
