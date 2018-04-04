// Test basic transaction write ops, reads, and commit using the shell helper.
// @tags: [requires_replication]
(function() {
    "use strict";
    const dbName = "test";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const testDB = rst.getPrimary().getDB(dbName);
    const coll = testDB.coll;

    if (!testDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    testDB.runCommand({create: coll.getName(), writeConcern: {w: "majority"}});

    jsTestLog("Setting session options");
    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);

    jsTestLog("Start transaction");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    // Performing a read first should work when snapshot readConcern is specified.
    assert.docEq(null, sessionDb.coll.findOne({_id: "insert-1"}));

    // Insert a doc within the transaction.
    assert.commandWorked(sessionDb.coll.insert({_id: "insert-1", a: 0}));

    // Insert a 2nd doc within the same transaction.
    assert.commandWorked(sessionDb.coll.insert({_id: "insert-2", a: 0}));

    // Insert a 3rd doc within the same transaction.
    assert.commandWorked(sessionDb.coll.insert({_id: "insert-3", a: 0}));

    // Update a document in the same transaction.
    assert.commandWorked(sessionDb.coll.update({_id: "insert-1"}, {$inc: {a: 1}}));

    // Delete a document in the same transaction.
    assert.commandWorked(sessionDb.coll.deleteOne({_id: "insert-2"}));

    // Try to find and modify a document within a transaction.
    sessionDb.coll.findAndModify(
        {query: {_id: "insert-3"}, update: {$set: {_id: "insert-3", a: 2}}});

    // Try to find a document within a transaction.
    let cursor = sessionDb.coll.find({_id: "insert-1"});
    assert.docEq({_id: "insert-1", a: 1}, cursor.next());

    // Try to find a document using findOne within a transaction
    assert.eq({_id: "insert-1", a: 1}, sessionDb.coll.findOne({_id: "insert-1"}));

    jsTestLog("Committing transaction.");
    session.commitTransaction();

    // Make sure the correct documents exist after committing the transaciton.
    assert.eq({_id: "insert-1", a: 1}, sessionDb.coll.findOne({_id: "insert-1"}));
    assert.eq({_id: "insert-3", a: 2}, sessionDb.coll.findOne({_id: "insert-3"}));
    assert.eq(null, sessionDb.coll.findOne({_id: "insert-2"}));

    jsTestLog("Start second transaction");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    // Insert a doc within the transaction.
    assert.commandWorked(sessionDb.coll.insert({_id: "insert-4", a: 0}));

    jsTestLog("Aborting transaction.");
    session.abortTransaction();

    // Verify that we cannot see the document we tried to insert.
    assert.eq(null, sessionDb.coll.findOne({_id: "insert-4"}));

    session.endSession();
    rst.stopSet();
}());
