/**
 * Tests prepared transactions can survive failover and commit on a new primary.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/replsets/rslib.js");  // For reconnect()

    const replTest = new ReplSetTest({nodes: 2});
    replTest.startSet();
    replTest.initiate();

    const dbName = jsTest.name();
    const collName = "coll";
    const otherDbName = dbName + "_other";

    function testTransactionsWithFailover(stepDownFunction) {
        const primary = replTest.getPrimary();
        const newPrimary = replTest.getSecondary();
        const testDB = primary.getDB(dbName);

        testDB.dropDatabase();
        testDB.getSiblingDB(otherDbName).dropDatabase();
        assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

        jsTestLog("Starting transaction");
        const session = primary.startSession({causalConsistency: false});
        const sessionDB = session.getDatabase(dbName);
        session.startTransaction({writeConcern: {w: "majority"}});

        const doc = {_id: "txn on primary " + primary};
        assert.commandWorked(sessionDB.getCollection(collName).insert(doc));

        jsTestLog("Putting transaction into prepare");
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
        replTest.awaitReplication();

        stepDownFunction();
        reconnect(primary);

        jsTestLog("Waiting for the other node to run for election and become primary");
        assert.eq(replTest.getPrimary(), newPrimary);

        jsTestLog("Creating an unrelated collection");
        // Application of an unrelated DDL command needs a strong lock on secondary. Make sure
        // the prepared transactions have yielded their locks on secondary.
        assert.commandWorked(newPrimary.getDB(otherDbName).runCommand({create: collName}));
        replTest.awaitReplication();

        jsTestLog("Dropping the collection in use cannot acquire the lock");
        assert.commandFailedWithCode(
            newPrimary.getDB(testDB).runCommand({drop: collName, maxTimeMS: 1000}),
            ErrorCodes.MaxTimeMSExpired);

        jsTestLog("Committing transaction on the new primary");
        // Create a proxy session to reuse the session state of the old primary.
        const newSession = new _DelegatingDriverSession(newPrimary, session);

        assert.commandWorked(
            PrepareHelpers.commitTransactionAfterPrepareTS(newSession, prepareTimestamp));
        replTest.awaitReplication();

        assert.docEq(doc, testDB.getCollection(collName).findOne());
        assert.docEq(doc, newPrimary.getDB(dbName).getCollection(collName).findOne());

        jsTestLog("Running another transaction on the new primary");
        const secondSession = newPrimary.startSession({causalConsistency: false});
        secondSession.startTransaction({writeConcern: {w: "majority"}});
        assert.commandWorked(
            secondSession.getDatabase(dbName).getCollection(collName).insert({_id: "second-doc"}));
        secondSession.commitTransaction();
    }

    function stepDownViaHeartbeat() {
        jsTestLog("Stepping down primary via heartbeat");
        replTest.stepUp(replTest.getSecondary());
    }
    testTransactionsWithFailover(stepDownViaHeartbeat);

    function stepDownViaCommand() {
        jsTestLog("Stepping down primary via command");
        assert.commandWorked(replTest.getPrimary().adminCommand({replSetStepDown: 10}));
    }
    testTransactionsWithFailover(stepDownViaCommand);

    replTest.stopSet();
})();
