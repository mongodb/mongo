/**
 * Test error cases when calling prepare on a non-existent transaction.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "prepare_nonexistent_transaction";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);

    testColl.drop({writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    const doc = {x: 1};

    jsTestLog("Test that if there is no transaction active on the current session, errors with " +
              "'NoSuchTransaction'.");
    assert.commandFailedWithCode(
        sessionDB.adminCommand(
            {prepareTransaction: 1, txnNumber: NumberLong(0), autocommit: false}),
        ErrorCodes.NoSuchTransaction);

    jsTestLog("Test that if there is a transaction running on the current session and the " +
              "'txnNumber' given is greater than the current transaction, errors with " +
              "'NoSuchTransaction'.");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert(doc));
    assert.commandFailedWithCode(sessionDB.adminCommand({
        prepareTransaction: 1,
        txnNumber: NumberLong(session.getTxnNumber_forTesting() + 1),
        autocommit: false
    }),
                                 ErrorCodes.NoSuchTransaction);
    session.abortTransaction_forTesting();

    session.startTransaction();
    assert.commandWorked(sessionColl.insert(doc));
    session.abortTransaction_forTesting();
    jsTestLog("Test that if there is no transaction active on the current session, the " +
              "'txnNumber' given matches the last known transaction for this session and the " +
              "last known transaction was aborted then it errors with 'NoSuchTransaction'.");
    assert.commandFailedWithCode(sessionDB.adminCommand({
        prepareTransaction: 1,
        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
        autocommit: false
    }),
                                 ErrorCodes.NoSuchTransaction);

    jsTestLog("Test that if there is a transaction running on the current session and the " +
              "'txnNumber' given is less than the current transaction, errors with " +
              "'TransactionTooOld'.");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert(doc));
    assert.commandFailedWithCode(
        sessionDB.adminCommand(
            {prepareTransaction: 1, txnNumber: NumberLong(0), autocommit: false}),
        ErrorCodes.TransactionTooOld);
    session.abortTransaction_forTesting();

    jsTestLog("Test that if there is no transaction active on the current session and the " +
              "'txnNumber' given is less than the current transaction, errors with " +
              "'TransactionTooOld'.");
    assert.commandFailedWithCode(
        sessionDB.adminCommand(
            {prepareTransaction: 1, txnNumber: NumberLong(0), autocommit: false}),
        ErrorCodes.TransactionTooOld);

    jsTestLog("Test the error precedence when calling prepare on a nonexistent transaction but " +
              "not providing txnNumber to prepareTransaction.");
    assert.commandFailedWithCode(sessionDB.adminCommand({prepareTransaction: 1, autocommit: false}),
                                 ErrorCodes.InvalidOptions);

    jsTestLog("Test the error precedence when calling prepare on a nonexistent transaction but " +
              "not providing autocommit to prepareTransaction.");
    assert.commandFailedWithCode(sessionDB.adminCommand({
        prepareTransaction: 1,
        txnNumber: NumberLong(session.getTxnNumber_forTesting() + 1),
    }),
                                 50768);

    jsTestLog("Test the error precedence when calling prepare on a nonexistent transaction and " +
              "providing startTransaction to prepareTransaction.");
    assert.commandFailedWithCode(sessionDB.adminCommand({
        prepareTransaction: 1,
        // The last txnNumber we used was saved on the server's session, so we use a txnNumber that
        // is greater than that to make sure it has never been seen before.
        txnNumber: NumberLong(session.getTxnNumber_forTesting() + 2),
        autocommit: false,
        startTransaction: true
    }),
                                 ErrorCodes.OperationNotSupportedInTransaction);

    session.endSession();
}());
