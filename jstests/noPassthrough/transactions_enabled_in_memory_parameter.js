/**
 * Verify that transactions can be run on the in-memory storage engine regardless of the
 * 'enableInMemoryTransactions' parameter setting -- it presently does nothing in the server.
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
    const collName = "transactions_enabled_in_memory_parameter";

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

    // Check that the default parameter value is false, but transactions work regardless.
    let res = primary.adminCommand({"getParameter": 1, "enableInMemoryTransactions": 1});
    assert.eq(res.enableInMemoryTransactions, false);

    // Try to start a transaction that should succeed.
    session.startTransaction();
    assert.commandWorked(sessionDb[collName].insert({x: 1}));
    session.commitTransaction();

    // Set the parameter to true, which will have no effect on in-memory transactions.
    assert.commandWorked(
        primary.adminCommand({"setParameter": 1, "enableInMemoryTransactions": true}));

    // Start a transaction that should succeed.
    session.startTransaction();
    assert.commandWorked(sessionDb[collName].insert({x: 2}));
    session.commitTransaction();

    session.endSession();
    replTest.stopSet();
}());
