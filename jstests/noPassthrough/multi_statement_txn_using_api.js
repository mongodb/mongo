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
    session.startTransaction();

    jsTestLog("Insert a doc within the transaction");
    assert.commandWorked(sessionDb.coll.insert({_id: "insert-1", a: 0}));

    jsTestLog("Insert a 2nd within the same transaction");
    assert.commandWorked(sessionDb.coll.insert({_id: "insert-2", a: 0}));

    jsTestLog("Insert a 3nd within the same transaction");
    assert.commandWorked(sessionDb.coll.insert({_id: "insert-3", a: 0}));

    jsTestLog("Update a document in the same transaction");
    assert.commandWorked(sessionDb.coll.update({_id: "insert-1"}, {$inc: {a: 1}}));

    jsTestLog("Delete a document in the same transaction");
    assert.commandWorked(sessionDb.coll.deleteOne({_id: "insert-2"}));

    jsTestLog("Try to find and modify a document within a transaction");
    sessionDb.coll.findAndModify(
        {query: {_id: "insert-3"}, update: {$set: {_id: "insert-3", a: 2}}});

    jsTestLog("Try to find a document within a transaction.");
    let cursor = sessionDb.coll.find({_id: "insert-1"});
    assert.docEq({_id: "insert-1", a: 1}, cursor.next());

    jsTestLog("Try to find a document using findOne within a transaction.");
    assert.eq({_id: "insert-1", a: 1}, sessionDb.coll.findOne({_id: "insert-1"}));

    jsTestLog("Committing transaction.");
    session.commitTransaction();

    assert.eq({_id: "insert-1", a: 1}, sessionDb.coll.findOne({_id: "insert-1"}));
    assert.eq({_id: "insert-3", a: 2}, sessionDb.coll.findOne({_id: "insert-3"}));
    assert.eq(null, sessionDb.coll.findOne({_id: "insert-2"}));

    session.endSession();
    rst.stopSet();
}());
