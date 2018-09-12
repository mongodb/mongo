/**
 * Test error cases when calling prepare on a committed transaction.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "prepare_committed_transaction";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);

    testColl.drop({writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    const doc = {x: 1};

    jsTestLog("Test that calling prepare on a committed transaction fails.");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert(doc));
    session.commitTransaction();
    assert.commandFailedWithCode(sessionDB.adminCommand({
        prepareTransaction: 1,
        coordinatorId: "dummy",
        txnNumber: NumberLong(0),
        autocommit: false
    }),
                                 ErrorCodes.TransactionCommitted);

    jsTestLog("Test the error precedence when calling prepare on a committed transaction but not " +
              "providing txnNumber to prepareTransaction.");
    assert.commandFailedWithCode(
        sessionDB.adminCommand({prepareTransaction: 1, coordinatorId: "dummy", autocommit: false}),
        ErrorCodes.InvalidOptions);

    jsTestLog("Test the error precedence when calling prepare on a committed transaction but not " +
              "providing autocommit to prepareTransaction.");
    assert.commandFailedWithCode(
        sessionDB.adminCommand(
            {prepareTransaction: 1, coordinatorId: "dummy", txnNumber: NumberLong(0)}),
        ErrorCodes.InvalidOptions);

    jsTestLog("Test the error precedence when calling prepare on a committed transaction and " +
              "providing startTransaction to prepareTransaction.");
    assert.commandFailedWithCode(sessionDB.adminCommand({
        prepareTransaction: 1,
        coordinatorId: "dummy",
        txnNumber: NumberLong(0),
        autocommit: false,
        startTransaction: true
    }),
                                 ErrorCodes.ConflictingOperationInProgress);

    session.endSession();
}());
