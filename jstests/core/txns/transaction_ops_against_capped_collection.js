/**
 * Tests that transactional writes are prohibited on capped collections, but transactional reads are
 * still allowed.
 *
 * 'requires_capped' tagged tests are excluded from txn passthrough suites.
 * @tags: [requires_capped, uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const cappedCollName = "transaction_ops_against_capped_collection";
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

    jsTest.log(
        "Starting individual transactions for writes against capped collections that should " +
        " fail.");

    /*
     * Write ops (should fail):
     */

    jsTest.log("About to try: insert");
    session.startTransaction();
    assert.commandFailedWithCode(sessionCappedColl.insert({"x": 55}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTest.log("About to try: update");
    session.startTransaction();
    assert.commandFailedWithCode(sessionCappedColl.update(testDocument, {"a": 1000}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTest.log("About to try: findAndModify (update version)");
    session.startTransaction();
    assert.commandFailedWithCode(
        sessionDB.runCommand(
            {findAndModify: cappedCollName, query: testDocument, update: {"$set": {"a": 1000}}}),
        ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTest.log("About to try: findAndModify (remove version)");
    session.startTransaction();
    assert.commandFailedWithCode(
        sessionDB.runCommand({findAndModify: cappedCollName, query: testDocument, remove: true}),
        ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // Deletes do not work against capped collections so we will not test them in transactions.

    jsTest.log(
        "Starting individual transactions for reads against capped collections that should " +
        " succeed.");

    /*
     * Read ops (should succeed):
     */

    jsTest.log("About to try: find");
    session.startTransaction();
    let findRes = assert.commandWorked(sessionDB.runCommand({"find": cappedCollName}));
    assert.eq(1, findRes.cursor.firstBatch[0].a);
    assert.commandWorked(session.abortTransaction_forTesting());

    jsTest.log("About to try: distinct");
    session.startTransaction();
    let distinctRes =
        assert.commandWorked(sessionDB.runCommand({"distinct": cappedCollName, "key": "a"}));
    assert.eq(1, distinctRes.values);
    assert.commandWorked(session.abortTransaction_forTesting());

    jsTest.log("About to try: aggregate");
    session.startTransaction();
    let aggRes = assert.commandWorked(sessionDB.runCommand({
        aggregate: cappedCollName,
        pipeline: [{$match: {"a": 1}}],
        cursor: {},
    }));
    assert.eq(1, aggRes.cursor.firstBatch[0].a);
    assert.commandWorked(session.abortTransaction_forTesting());

    session.endSession();
})();
