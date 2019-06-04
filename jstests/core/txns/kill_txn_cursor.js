// Tests that killing a cursor created in a transaction does not abort the transaction.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "kill_txn_cursor";
    const testDB = db.getSiblingDB(dbName);

    const sessionOptions = {causalConsistency: false};
    const session = db.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    const bulk = sessionColl.initializeUnorderedBulkOp();
    for (let i = 0; i < 4; ++i) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));

    jsTest.log("Start a transaction.");
    session.startTransaction({writeConcern: {w: "majority"}});

    // Open cursor 1, and do not exhaust the cursor.
    let cursorRes1 = assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));
    assert(cursorRes1.hasOwnProperty("cursor"), tojson(cursorRes1));
    assert(cursorRes1.cursor.hasOwnProperty("id"), tojson(cursorRes1));
    let cursorId1 = cursorRes1.cursor.id;
    jsTest.log("Opened cursor 1 with id " + cursorId1);

    // Open cursor 2, and do not exhaust the cursor.
    let cursorRes2 = assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));
    assert(cursorRes2.hasOwnProperty("cursor"), tojson(cursorRes2));
    assert(cursorRes2.cursor.hasOwnProperty("id"), tojson(cursorRes2));
    let cursorId2 = cursorRes2.cursor.id;
    jsTest.log("Opened cursor 2 with id " + cursorId2);

    jsTest.log("Kill cursor 1 outside of the transaction.");
    // Kill cursor 1. We check that the kill was successful by asserting that the killCursors
    // command worked. We could run a getMore and check that we get a CursorNotFound error, but this
    // error would abort the transaction and kill cursor 2, and we want to check that cursor 2 is
    // still alive.
    assert.commandWorked(testDB.runCommand({killCursors: collName, cursors: [cursorId1]}));

    jsTest.log("Cursor 2 is still alive.");
    cursorRes2 =
        assert.commandWorked(sessionDb.runCommand({getMore: cursorId2, collection: collName}));
    assert(cursorRes2.hasOwnProperty("cursor"));
    assert(cursorRes2.cursor.hasOwnProperty("nextBatch"));
    assert.sameMembers(cursorRes2.cursor.nextBatch, [{_id: 2}, {_id: 3}]);

    jsTest.log("Can still write in the transaction");
    assert.commandWorked(sessionColl.insert({_id: 4}));

    jsTest.log("Commit transaction.");
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.sameMembers([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}],
                       sessionColl.find().toArray());

    session.endSession();
}());
