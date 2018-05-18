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

    // Try to start a transaction with one already running.
    session.startTransaction();
    try {
        session.startTransaction();
    } catch (e) {
        assert.eq(e.message, "Transaction already in progress on this session.");
    }

    session.endSession();
}());
