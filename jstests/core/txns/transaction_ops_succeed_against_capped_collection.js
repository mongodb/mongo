/**
 * Tests that transaction CRUD operations on a capped collection against a non-shard replica set is
 * allowed and succeeds.
 *
 * 'requires_capped' tagged tests are excluded from sharding txn passthrough suites.
 * @tags: [requires_capped, uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const cappedCollName = "transaction_ops_succeed_against_capped_collection";
    const testDB = db.getSiblingDB(dbName);
    const cappedTestColl = testDB.getCollection(cappedCollName);
    const testDocument = {"a": "docToTryToUpdateThenDelete"};

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
    session.startTransaction();

    jsTest.log("Running an insert op in the transaction against a capped collection that should " +
               "succeed");
    assert.commandWorked(sessionCappedColl.insert({x: 55}));

    jsTest.log("Running an update op in the transaction against a capped collection that should " +
               "succeed");
    assert.commandWorked(
        sessionCappedColl.update(testDocument, {"a": "docIsGettingUpdatedAndSize"}));

    // Deletes do not work against capped collections so we will not test it in a transaction.

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();
})();
