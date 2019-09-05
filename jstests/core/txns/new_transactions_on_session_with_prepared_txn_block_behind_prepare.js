/**
 * Tests that new transactions on a session block behind an existing prepared transaction on the
 * session.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/curop_helpers.js");  // for waitForCurOpByFailPoint().
load("jstests/libs/parallel_shell_helpers.js");

/**
 * Launches a parallel shell to start a new transaction on the session with the given lsid. It
 * performs an insert and then commits. Assumes that there will be an already-prepared transaction
 * on the session, and blocks using a failpoint until the transaction in the parallel shell has
 * begun to block behind the prepared transaction.
 */
function runConcurrentTransactionOnSession(dbName, collName, lsid) {
    var awaitShell;
    try {
        // Turn on failpoint that parallel shell will hit when blocked on prepare.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: 'waitAfterNewStatementBlocksBehindPrepare', mode: "alwaysOn"}));

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
        waitForCurOpByFailPointNoNS(db, "waitAfterNewStatementBlocksBehindPrepare");
    } finally {
        // Disable failpoint to allow the parallel shell to continue - it should still be blocked on
        // prepare. This is needed in a finally block so that if something fails we're guaranteed to
        // turn this failpoint off, so that it doesn't cause problems for subsequent tests.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: 'waitAfterNewStatementBlocksBehindPrepare', mode: "off"}));
    }

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

(() => {
    jsTestLog(
        "New transactions on a session should block behind an existing prepared transaction on that session until it aborts.");

    const session = testDB.getMongo().startSession();
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);
    const lsid = session.getSessionId();

    // Start and prepare a transaction.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({x: "foo"}));
    PrepareHelpers.prepareTransaction(session);

    // Launch a concurrent transaction which should block behind the active prepared transaction.
    const awaitShell = runConcurrentTransactionOnSession(dbName, collName, lsid);

    // Abort the original transaction - this should allow the parallel shell to continue and start a
    // new transaction.
    assert.commandWorked(session.abortTransaction_forTesting());

    awaitShell();

    session.endSession();
})();

(() => {
    jsTestLog(
        "New transactions on a session should block behind an existing prepared transaction on that session until it commits.");

    const session = testDB.getMongo().startSession();
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);
    const lsid = session.getSessionId();

    // Start and prepare a transaction.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({x: "foo"}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    // Launch a concurrent transaction which should block behind the active prepared transaction.
    const awaitShell = runConcurrentTransactionOnSession(dbName, collName, lsid);

    // Commit the original transaction - this should allow the parallel shell to continue and start
    // a new transaction. Not using PrepareHelpers.commitTransaction because it calls
    // commitTransaction twice, and the second call races with the second transaction the test
    // started.
    assert.commandWorked(session.getDatabase('admin').adminCommand(
        {commitTransaction: 1, commitTimestamp: prepareTimestamp}));

    awaitShell();

    session.endSession();
})();

(() => {
    jsTestLog(
        "Test error precedence when executing a malformed command during a prepared transaction.");

    const session = testDB.getMongo().startSession();
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-1"}));
    PrepareHelpers.prepareTransaction(session);

    // The following command specifies txnNumber: 2 without startTransaction: true.
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "no_such_txn"}],
        txnNumber: NumberLong(2),
        stmtId: NumberInt(0),
        autocommit: false
    }),
                                 ErrorCodes.NoSuchTransaction);

    assert.commandWorked(session.abortTransaction_forTesting());

    session.endSession();
})();
}());
