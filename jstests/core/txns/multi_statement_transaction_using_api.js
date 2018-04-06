// Test basic transaction write ops, reads, and commit/abort using the shell helper.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "multi_transaction_test_using_api";
    const testDB = db.getSiblingDB(dbName);

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    jsTestLog("Start transaction");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    // Performing a read first should work when snapshot readConcern is specified.
    assert.docEq(null, sessionColl.findOne({_id: "insert-1"}));

    assert.commandWorked(sessionColl.insert({_id: "insert-1", a: 0}));

    assert.commandWorked(sessionColl.insert({_id: "insert-2", a: 0}));

    assert.commandWorked(sessionColl.insert({_id: "insert-3", a: 0}));

    assert.commandWorked(sessionColl.update({_id: "insert-1"}, {$inc: {a: 1}}));

    assert.commandWorked(sessionColl.deleteOne({_id: "insert-2"}));

    sessionColl.findAndModify({query: {_id: "insert-3"}, update: {$set: {a: 2}}});

    // Try to find a document within a transaction.
    let cursor = sessionColl.find({_id: "insert-1"});
    assert.docEq({_id: "insert-1", a: 1}, cursor.next());
    assert(!cursor.hasNext());

    // Try to find a document using findOne within a transaction
    assert.eq({_id: "insert-1", a: 1}, sessionColl.findOne({_id: "insert-1"}));

    jsTestLog("Committing transaction.");
    session.commitTransaction();

    // Make sure the correct documents exist after committing the transaciton.
    assert.eq({_id: "insert-1", a: 1}, sessionColl.findOne({_id: "insert-1"}));
    assert.eq({_id: "insert-3", a: 2}, sessionColl.findOne({_id: "insert-3"}));
    assert.eq(null, sessionColl.findOne({_id: "insert-2"}));

    jsTestLog("Start second transaction");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    assert.commandWorked(sessionColl.insert({_id: "insert-4", a: 0}));

    jsTestLog("Aborting transaction.");
    session.abortTransaction();

    // Verify that we cannot see the document we tried to insert.
    assert.eq(null, sessionColl.findOne({_id: "insert-4"}));

    session.endSession();
}());
