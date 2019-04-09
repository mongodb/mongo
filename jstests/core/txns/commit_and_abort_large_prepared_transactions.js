/**
 * Tests support for prepared transactions larger than 16MB.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const collName = "large_prepared_transactions";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);

    const paramResult =
        testDB.adminCommand({"getParameter": 1, useMultipleOplogEntryFormatForTransactions: 1});
    if (!paramResult["useMultipleOplogEntryFormatForTransactions"]) {
        // TODO: SERVER-39810 Remove this early return once the new oplog format for large
        // transactions is made the default.
        jsTestLog(
            "Skipping the test because useMultipleOplogEntryFormatForTransactions is not set to true.");
        return;
    }

    // As we are not able to send a single request larger than 16MB, we insert two documents
    // of 10MB each to create a "large" transaction.
    const kSize10MB = 10 * 1024 * 1024;
    function createLargeDocument(id) {
        return {_id: id, longString: new Array(kSize10MB).join("a")};
    }

    testColl.drop({writeConcern: {w: "majority"}});
    assert.commandWorked(
        testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    // Test preparing and committing a large transaction with two 10MB inserts.
    let doc1 = createLargeDocument(1);
    let doc2 = createLargeDocument(2);
    session.startTransaction();
    assert.commandWorked(sessionColl.insert(doc1));
    assert.commandWorked(sessionColl.insert(doc2));

    let prepareTimestamp = PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
    assert.sameMembers(sessionColl.find().toArray(), [doc1, doc2]);

    // Test preparing and aborting a large transaction with two 10MB inserts.
    let doc3 = createLargeDocument(3);
    let doc4 = createLargeDocument(4);
    session.startTransaction();
    assert.commandWorked(sessionColl.insert(doc3));
    assert.commandWorked(sessionColl.insert(doc4));

    PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(session.abortTransaction_forTesting());
    assert.sameMembers(sessionColl.find({_id: {$gt: 2}}).toArray(), []);
}());
