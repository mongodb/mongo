/**
 * Verify that transactions can be run on the in-memory storage engine. inMemory transactions are
 * not fully supported, but should work for basic MongoDB user testing.
 *
 * TODO: remove this test when general transaction testing is turned on with the inMemory storage
 * engine (SERVER-36023).
 */
(function() {
    "use strict";

    if (jsTest.options().storageEngine !== "inMemory") {
        jsTestLog("Skipping test because storageEngine is not inMemory");
        return;
    }

    const dbName = "test";
    const collName = "transactions_work_with_in_memory_engine";

    const replTest = new ReplSetTest({name: collName, nodes: 1});
    replTest.startSet({storageEngine: "inMemory"});
    replTest.initiate();

    const primary = replTest.getPrimary();

    // Initiate a session.
    const sessionOptions = {causalConsistency: false};
    const session = primary.getDB(dbName).getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);

    // Create collection.
    assert.commandWorked(sessionDb[collName].insert({x: 0}));

    // Execute a transaction that should succeed.
    session.startTransaction();
    assert.commandWorked(sessionDb[collName].insert({x: 1}));
    assert.commandWorked(session.commitTransaction_forTesting());

    session.endSession();
    replTest.stopSet();
}());
