// Tests that cursors created in transactions may be killed outside of the transaction.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "kill_transaction_cursors";
    const testDB = db.getSiblingDB(dbName);
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    sessionColl.drop();
    for (let i = 0; i < 4; ++i) {
        assert.commandWorked(sessionColl.insert({_id: i}));
    }

    jsTest.log("Test that cursors created in transactions may be kill outside of the transaction.");
    session.startTransaction();
    let res = assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));
    assert(res.hasOwnProperty("cursor"), tojson(res));
    assert(res.cursor.hasOwnProperty("id"), tojson(res));
    session.commitTransaction();
    assert.commandWorked(sessionDb.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

    jsTest.log("Test that cursors created in transactions may be kill outside of the session.");
    session.startTransaction();
    res = assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));
    assert(res.hasOwnProperty("cursor"), tojson(res));
    assert(res.cursor.hasOwnProperty("id"), tojson(res));
    session.commitTransaction();
    assert.commandWorked(testDB.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

    session.endSession();
}());
