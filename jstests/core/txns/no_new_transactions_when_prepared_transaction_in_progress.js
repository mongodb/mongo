/**
 * Tests that we cannot start a new transaction when a prepared transaction exists on the session.
 * @tags: [uses_transactions, uses_prepare_transaction]
 *
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const collName = "no_new_transactions_when_prepared_transaction_in_progress";
    const testDB = db.getSiblingDB(dbName);

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    jsTestLog(
        "Test starting a new transaction while an existing prepared transaction exists on the " +
        "session.");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-1"}));
    PrepareHelpers.prepareTransaction(session);
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "cannot_start"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(1),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }),
                                 ErrorCodes.PreparedTransactionInProgress);

    jsTestLog(
        "Test error precedence when executing a malformed command during a prepared transaction.");
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
}());
