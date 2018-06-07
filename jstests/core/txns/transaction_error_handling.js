// Test basic transaction error handling.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "transaction_error_handling";
    const testDB = db.getSiblingDB(dbName);

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    jsTestLog("Test that we cannot abort or commit a nonexistant transaction.");
    // Cannot abort or commit a nonexistant transaction.
    try {
        session.commitTransaction();
    } catch (e) {
        assert.eq(e.message, "There is no active transaction to commit on this session.");
    }

    try {
        session.abortTransaction();
    } catch (e) {
        assert.eq(e.message, "There is no active transaction to abort on this session.");
    }

    // Try to start a transaction when the state is 'active'.
    jsTestLog("Test that we cannot start a transaction with one already started or in progress.");
    session.startTransaction();
    try {
        session.startTransaction();
    } catch (e) {
        assert.eq(e.message, "Transaction already in progress on this session.");
    }

    // Try starting a transaction after inserting something.
    assert.commandWorked(sessionColl.insert({_id: "insert-1"}));
    // Try to start a transaction when the state is 'active'.
    try {
        session.startTransaction();
    } catch (e) {
        assert.eq(e.message, "Transaction already in progress on this session.");
    }

    // At this point, the transaction is still 'active'. We will commit this transaction and test
    // that calling commitTransaction again should work while calling abortTransaction should not.
    session.commitTransaction();

    jsTestLog("Test that we can commit a transaction more than once.");
    // The transaction state is 'committed'. We can call commitTransaction again in this state.
    session.commitTransaction();

    jsTestLog("Test that we cannot abort a transaction that has already been committed");
    // We cannot call abortTransaction on a transaction that has already been committed.
    try {
        session.abortTransaction();
    } catch (e) {
        assert.eq(e.message, "Cannot call abortTransaction after calling commitTransaction.");
    }

    // Start a new transaction that will be aborted. Test that we cannot call commit or
    // abortTransaction on a transaction that is in the 'aborted' state.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-2"}));
    session.abortTransaction();

    jsTestLog("Test that we cannot commit a transaction that has already been aborted.");
    // We cannot call commitTransaction on a transaction that has already been aborted.
    try {
        session.commitTransaction();
    } catch (e) {
        assert.eq(e.message, "Cannot call commitTransaction after calling abortTransaction.");
    }

    jsTestLog("Test that we cannot abort a transaction that has already been aborted.");
    // We also cannot call abortTransaction on a transaction that has already been aborted.
    try {
        session.abortTransaction();
    } catch (e) {
        assert.eq(e.message, "Cannot call abortTransaction twice.");
    }

    jsTestLog(
        "Test that a normal operation after committing a transaction changes the state to inactive.");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-3"}));
    // The transaction state should be changed to 'committed'.
    session.commitTransaction();
    // The transaction state should be changed to 'inactive'.
    assert.commandWorked(sessionColl.insert({_id: "normal-insert"}));
    try {
        session.commitTransaction();
    } catch (e) {
        assert.eq(e.message, "There is no active transaction to commit on this session.");
    }

    jsTestLog(
        "Test that a normal operation after aborting a transaction changes the state to inactive.");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-4"}));
    // The transaction state should be changed to 'aborted'.
    session.abortTransaction();
    // The transaction state should be changed to 'inactive'.
    assert.commandWorked(sessionColl.insert({_id: "normal-insert-2"}));
    try {
        session.abortTransaction();
    } catch (e) {
        assert.eq(e.message, "There is no active transaction to abort on this session.");
    }

    session.endSession();
}());
