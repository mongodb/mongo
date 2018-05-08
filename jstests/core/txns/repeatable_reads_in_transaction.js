// Test to guarantee read repeatability, meaning that while in a transaction, we should repeatedly
// read the same data even if it was modified outside of the transaction.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "repeatable_reads_in_transaction";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB[collName];

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

    const sessionOptions = {causalConsistency: false};
    const session = db.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    // Initialize second session variables.
    const session2 = testDB.getMongo().startSession(sessionOptions);
    const session2Db = session2.getDatabase(dbName);
    const session2Coll = session2Db.getCollection(collName);

    jsTest.log("Prepopulate the collection.");
    assert.writeOK(
        testColl.insert([{_id: 0}, {_id: 1}, {_id: 2}], {writeConcern: {w: "majority"}}));

    // Create a constant array of documents we expect to be returned during a read-only transaction.
    // The value should not change since external changes should not be visible within this
    // transaction.
    const expectedDocs = [{_id: 0}, {_id: 1}, {_id: 2}];

    jsTestLog("Start a read-only transaction on the first session.");
    session.startTransaction({writeConcern: {w: "majority"}});

    assert.docEq(expectedDocs, sessionColl.find().toArray());

    jsTestLog("Start a transaction on the second session that modifies the same collection.");
    session2.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    assert.commandWorked(session2Coll.insert({_id: 3}));
    assert.commandWorked(session2Coll.update({_id: 1}, {$set: {a: 1}}));
    assert.commandWorked(session2Coll.deleteOne({_id: 2}));

    jsTestLog(
        "Continue reading in the first transaction. Changes from the second transaction should not be visible.");

    assert.docEq(expectedDocs, sessionColl.find().toArray());

    jsTestLog("Committing the second transaction.");
    session2.commitTransaction();

    jsTestLog(
        "Committed changes from the second transaction should still not be visible to the first.");

    assert.docEq(expectedDocs, sessionColl.find().toArray());

    jsTestLog(
        "Writes that occur outside of a transaction should not be visible to a read only transaction.");

    assert.writeOK(testColl.insert({_id: 4}, {writeConcern: {w: "majority"}}));

    assert.docEq(expectedDocs, sessionColl.find().toArray());

    jsTestLog("Committing first transaction.");
    session.commitTransaction();

    // Make sure the correct documents exist after committing the second transaction.
    assert.docEq([{_id: 0}, {_id: 1, a: 1}, {_id: 3}, {_id: 4}], sessionColl.find().toArray());

    session.endSession();
    session2.endSession();
}());