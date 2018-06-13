// Tests that killSessions kills inactive transactions.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "kill_sessions_kills_transaction";
    const testDB = db.getSiblingDB(dbName);
    const adminDB = db.getSiblingDB("admin");
    const testColl = testDB[collName];
    const sessionOptions = {causalConsistency: false};

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    const bulk = testColl.initializeUnorderedBulkOp();
    for (let i = 0; i < 4; ++i) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));

    jsTest.log("Test that killing a session kills an inactive transaction.");
    let session = db.getMongo().startSession(sessionOptions);
    let sessionDb = session.getDatabase(dbName);
    let sessionColl = sessionDb[collName];

    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: 5}));
    assert.commandWorked(testDB.runCommand({killSessions: [session.getSessionId()]}));
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.endSession();

    jsTest.log("killSessions must not block on locks held by a transaction it plans to kill.");
    session = db.getMongo().startSession(sessionOptions);
    sessionDb = session.getDatabase(dbName);
    sessionColl = sessionDb[collName];

    session.startTransaction();
    // Open a cursor on the collection.
    assert.commandWorked(sessionDb.runCommand({find: collName, batchSize: 2}));

    // Start a drop, which will hang.
    let awaitDrop = startParallelShell(function() {
        db.getSiblingDB("test")["kill_sessions_kills_transaction"].drop();
    });

    // Wait for the drop to have a pending MODE_X lock on the database.
    assert.soon(
        function() {
            return adminDB
                       .aggregate([
                           {$currentOp: {}},
                           {$match: {"command.drop": collName, waitingForLock: true}}
                       ])
                       .itcount() === 1;
        },
        function() {
            return "Failed to find drop in currentOp output: " +
                tojson(adminDB.aggregate([{$currentOp: {}}]).toArray());
        });

    // killSessions needs to acquire a MODE_IS lock on the collection in order to kill the open
    // cursor. However, the transaction is holding a MODE_IX lock on the collection, which will
    // block the drop from obtaining a MODE_X lock on the database, which will block the
    // killSessions from taking a MODE_IS lock on the collection. In order to avoid hanging,
    // killSessions must first kill the transaction, so that it releases its MODE_IX collection
    // lock. This allows the drop to proceed and obtain and release the MODE_X lock. Finally,
    // killSessions can obtain a MODE_IS collection lock and kill the cursor.
    assert.commandWorked(testDB.runCommand({killSessions: [session.getSessionId()]}));
    awaitDrop();
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.endSession();
}());
