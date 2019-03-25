// Test that a transaction which has done only a no-op write (an update with no effect), and aborts,
// awaits the system lastOpTime with the transaction's writeConcern.
// @tags: [uses_transactions]
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");  // For stopReplicationOnSecondaries.

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const session = primary.getDB("admin").getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);

    assert.commandWorked(primaryDB[collName].insert({x: 1}, {writeConcern: {w: "majority"}}));

    jsTestLog("Stop replication");

    stopReplicationOnSecondaries(rst);

    jsTestLog("Start snapshot transaction, initial command is a write with no effect");

    session.startTransaction(
        {writeConcern: {w: "majority", wtimeout: 1000}, readConcern: {level: "snapshot"}});

    const fruitlessUpdate = {update: collName, updates: [{q: {x: 1}, u: {$set: {x: 1}}}]};
    printjson(assert.commandWorked(sessionDB.runCommand(fruitlessUpdate)));

    jsTestLog("Advance opTime on primary, with replication stopped");

    printjson(assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{}]})));

    jsTestLog("Abort the transaction, expect wtimeout after 1 second");

    assert.commandFailedWithCode(
        session.abortTransaction_forTesting(), ErrorCodes.WriteConcernFailed, "abort transaction");

    jsTestLog("Restart replication");

    restartReplicationOnSecondaries(rst);

    jsTestLog("Try transaction with replication enabled");

    session.startTransaction({
        writeConcern: {w: "majority", wtimeout: ReplSetTest.kDefaultTimeoutMS},
        readConcern: {level: "snapshot"}
    });

    assert.commandWorked(sessionDB.runCommand(fruitlessUpdate));
    assert.commandWorked(session.abortTransaction_forTesting());

    rst.stopSet();
}());
