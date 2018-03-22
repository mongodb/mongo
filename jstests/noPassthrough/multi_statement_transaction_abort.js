// Test basic transaction aborts with two inserts.
// @tags: [requires_replication]
(function() {
    "use strict";
    load('jstests/libs/uuid_util.js');

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const testDB = rst.getPrimary().getDB(dbName);
    const coll = testDB.coll;

    if (!testDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    assert.commandWorked(
        testDB.runCommand({create: coll.getName(), writeConcern: {w: "majority"}}));
    let txnNumber = 0;

    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);

    jsTest.log("Insert two documents in a transaction and abort");

    // Insert a doc within the transaction.
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-1"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        // Only the first write in a transaction has autocommit flag.
        autocommit: false
    }));

    // Insert a doc within a transaction.
    assert.commandWorked(sessionDb.runCommand(
        {insert: collName, documents: [{_id: "insert-2"}], txnNumber: NumberLong(txnNumber)}));

    // Cannot read with default read concern.
    assert.eq(null, testDB.coll.findOne({_id: "insert-1"}));
    // Cannot read with default read concern.
    assert.eq(null, testDB.coll.findOne({_id: "insert-2"}));

    assert.commandWorked(sessionDb.runCommand(
        {abortTransaction: 1, writeConcern: {w: "majority"}, txnNumber: NumberLong(txnNumber)}));

    // Read with default read concern cannot see the aborted transaction.
    assert.eq(null, testDB.coll.findOne({_id: "insert-1"}));
    assert.eq(null, testDB.coll.findOne({_id: "insert-2"}));

    jsTest.log("Insert two documents in a transaction and commit");

    // Insert a doc with the same _id's in a new transaction should work.
    txnNumber++;
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-1"}, {_id: "insert-2"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }));
    assert.commandWorked(sessionDb.runCommand(
        {commitTransaction: 1, writeConcern: {w: "majority"}, txnNumber: NumberLong(txnNumber)}));
    // Read with default read concern sees the committed transaction.
    assert.eq({_id: "insert-1"}, testDB.coll.findOne({_id: "insert-1"}));
    assert.eq({_id: "insert-2"}, testDB.coll.findOne({_id: "insert-2"}));

    jsTest.log("Cannot abort empty transaction because it's not in progress");
    txnNumber++;
    assert.commandFailedWithCode(
        sessionDb.runCommand(
            {abortTransaction: 1, writeConcern: {w: "majority"}, txnNumber: NumberLong(txnNumber)}),
        [ErrorCodes.CommandFailed]);

    jsTest.log("Higher transaction number aborts existing running transaction.");
    txnNumber++;
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "running-txn-1"}, {_id: "running-txn-2"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }));
    // A higher txnNumber aborts the old and inserts the same document.
    txnNumber++;
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "running-txn-2"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }));
    assert.commandWorked(sessionDb.runCommand(
        {commitTransaction: 1, writeConcern: {w: "majority"}, txnNumber: NumberLong(txnNumber)}));
    // Read with default read concern sees the committed transaction but cannot see the aborted one.
    assert.eq(null, testDB.coll.findOne({_id: "running-txn-1"}));
    assert.eq({_id: "running-txn-2"}, testDB.coll.findOne({_id: "running-txn-2"}));

    jsTest.log("Higher transaction number aborts existing running snapshot read.");
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(
        coll.insert([{doc: 1}, {doc: 2}, {doc: 3}], {writeConcern: {w: "majority"}}));
    txnNumber++;
    // Perform a snapshot read under a new transaction.
    let runningReadResult = assert.commandWorked(sessionDb.runCommand({
        find: collName,
        batchSize: 2,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }));

    // The cursor has not been exhausted.
    assert(runningReadResult.hasOwnProperty("cursor"), tojson(runningReadResult));
    assert.neq(0, runningReadResult.cursor.id, tojson(runningReadResult));

    txnNumber++;
    // Perform a second snapshot read under a new transaction.
    let newReadResult = assert.commandWorked(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }));

    // The cursor has been exhausted.
    assert(newReadResult.hasOwnProperty("cursor"), tojson(newReadResult));
    assert.eq(0, newReadResult.cursor.id, tojson(newReadResult));
    assert.commandWorked(sessionDb.runCommand(
        {commitTransaction: 1, writeConcern: {w: "majority"}, txnNumber: NumberLong(txnNumber)}));

    // TODO: SERVER-33690 Test the old cursor has been killed when the transaction is aborted.

    session.endSession();
    rst.stopSet();
}());
