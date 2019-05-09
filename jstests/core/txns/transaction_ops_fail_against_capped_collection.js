/**
 * Tests that transaction CRUD operations are not allowed on capped collections.
 *
 * 'requires_capped' tagged tests are excluded from txn passthrough suites.
 * @tags: [requires_capped, uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const cappedCollName = "transaction_ops_fail_against_capped_collection";
    const testDB = db.getSiblingDB(dbName);
    const cappedTestColl = testDB.getCollection(cappedCollName);
    const testDocument = {"a": 1};

    cappedTestColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Creating a capped collection '" + dbName + "." + cappedCollName + "'.");
    assert.commandWorked(testDB.createCollection(cappedCollName, {capped: true, size: 500}));

    jsTest.log("Adding a document to the capped collection so that the update op can be tested " +
               "in the subsequent transaction attempts");
    assert.commandWorked(cappedTestColl.insert(testDocument));

    jsTest.log("Setting up a transaction in which to execute transaction ops.");
    const session = db.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionCappedColl = sessionDB.getCollection(cappedCollName);

    jsTest.log("Starting a transaction for an insert op against a capped collection that should " +
               "fail");
    session.startTransaction();
    assert.commandFailedWithCode(sessionCappedColl.insert({"x": 55}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTest.log("Starting a transaction for an update op against a capped collection that should " +
               "fail");
    session.startTransaction();
    assert.commandFailedWithCode(sessionCappedColl.update(testDocument, {"a": 1000}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // Deletes do not work against capped collections so we will not test it in a transaction.

    session.endSession();
})();
