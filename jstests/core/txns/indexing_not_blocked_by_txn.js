/*
 * This test ensures that creating and dropping an index on a collection
 * does not conflict with multi-statement transactions on different collections
 * as a result of taking strong database locks. Additionally tests that a
 * createIndexes for an index that already exist does not conflict with an open
 * transaction on that same collection.
 * @tags: [uses_transactions, assumes_unsharded_collection]
 */
(function() {
    "use strict";
    var dbName = 'indexing_not_blocked_by_txn';
    var mydb = db.getSiblingDB(dbName);
    const wcMajority = {writeConcern: {w: "majority"}};

    mydb.foo.drop(wcMajority);
    mydb.bar.drop(wcMajority);
    assert.commandWorked(mydb.createCollection("foo", wcMajority));
    assert.commandWorked(mydb.foo.createIndex({x: 1}));
    assert.commandWorked(mydb.createCollection("bar", wcMajority));

    var session = db.getMongo().startSession();
    var sessionDb = session.getDatabase(dbName);

    session.startTransaction();
    assert.commandWorked(sessionDb.foo.insert({x: 1}));

    // Creating already existing index is a no-op that shouldn't take strong locks.
    assert.commandWorked(mydb.foo.createIndex({x: 1}));

    // Creating an index on a different collection should not conflict.
    assert.commandWorked(mydb.bar.createIndex({x: 1}));
    // Dropping shouldn't either.
    assert.commandWorked(mydb.bar.dropIndex({x: 1}));

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();
}());
