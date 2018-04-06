// Test transaction starting with read.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "start_transaction_with_read";

    const testDB = db.getSiblingDB(dbName);
    const coll = testDB[collName];

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    testDB.runCommand({create: coll.getName(), writeConcern: {w: "majority"}});
    let txnNumber = 0;

    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    // Non-transactional write to give something to find.
    const initialDoc = {_id: "pretransaction1", x: 0};
    assert.writeOK(sessionColl.insert(initialDoc, {writeConcern: {w: "majority"}}));

    jsTest.log("Start a transaction with a read");
    let res = assert.commandWorked(sessionDb.runCommand({
        find: collName,
        batchSize: 10,
        txnNumber: NumberLong(txnNumber),
        readConcern: {level: "snapshot"},
        startTransaction: true,
        autocommit: false
    }));
    assert.eq(res.cursor.firstBatch, [initialDoc]);

    jsTest.log("Insert two documents in a transaction");
    // Insert a doc within the transaction.
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-1"}],
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }));

    // Read in the same transaction returns the doc.
    res = sessionDb.runCommand({
        find: collName,
        filter: {_id: "insert-1"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    });
    assert.commandWorked(res);
    assert.docEq([{_id: "insert-1"}], res.cursor.firstBatch);

    // Insert a doc within a transaction.
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-2"}],
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }));

    // commitTransaction can only be run on the admin database.
    assert.commandWorked(sessionDb.adminCommand(
        {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));

    // Read with default read concern sees the committed transaction.
    assert.eq({_id: "insert-1"}, coll.findOne({_id: "insert-1"}));
    assert.eq({_id: "insert-2"}, coll.findOne({_id: "insert-2"}));
    assert.eq(initialDoc, coll.findOne(initialDoc));

    session.endSession();
}());
