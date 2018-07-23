/**
 * Verify that transactions can be run on the in-memory storage engine with the
 * 'enableInMemoryTransactions' parameter.
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

    // Check the default parameter value.
    let res = primary.adminCommand({"getParameter": 1, "enableInMemoryTransactions": 1});
    assert.eq(res.enableInMemoryTransactions, false);

    // Try to start a transaction that should fail.
    session.startTransaction();
    assert.commandFailedWithCode(sessionDb[collName].insert({x: 1}), ErrorCodes.IllegalOperation);
    // The abort command will fail on the server, but should reset the local transaction state
    // appropriately.
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.IllegalOperation);

    // Enable in-memory transactions.
    assert.commandWorked(
        primary.adminCommand({"setParameter": 1, "enableInMemoryTransactions": true}));

    // Start a transaction that should succeed.
    session.startTransaction();
    assert.commandWorked(sessionDb[collName].insert({x: 2}));
    session.commitTransaction();

    session.endSession();
    replTest.stopSet();
}());
