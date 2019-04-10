/**
 * Tests that prepare transaction fails if the transaction operated on a temporary collection.
 *
 * Transactions should not operate on temporary collections because they are for internal use only
 * and are deleted on both repl stepup and server startup.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";

    const dbName = "test";
    const tempCollName = "prepare_transaction_fails_on_temp_collections";
    const testDB = db.getSiblingDB(dbName);
    const testTempColl = testDB.getCollection(tempCollName);

    testTempColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Creating a temporary collection.");
    assert.commandWorked(testDB.runCommand({
        applyOps:
            [{op: "c", ns: testDB.getName() + ".$cmd", o: {create: tempCollName, temp: true}}]
    }));

    const session = db.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionTempColl = sessionDB.getCollection(tempCollName);

    jsTest.log("Setting up a transaction with an operation on a temporary collection.");
    session.startTransaction();
    assert.commandWorked(sessionTempColl.insert({x: 1000}));

    jsTest.log("Calling prepareTransaction for a transaction with operations against a " +
               "temporary collection should now fail.");
    assert.commandFailedWithCode(sessionDB.adminCommand({prepareTransaction: 1}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
})();
