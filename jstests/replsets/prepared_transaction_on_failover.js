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

    function testTransactionsWithFailover(doWork, stepDown, postCommit) {
        const primary = replTest.getPrimary();
        const newPrimary = replTest.getSecondary();
        const testDB = primary.getDB(dbName);

        testDB.dropDatabase();
        testDB.getSiblingDB(otherDbName).dropDatabase();
        assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

        jsTestLog("Starting transaction");
        const session = primary.startSession({causalConsistency: false});
        session.startTransaction({writeConcern: {w: "majority"}});

        doWork(primary, session);

        jsTestLog("Putting transaction into prepare");
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
        replTest.awaitReplication();

        stepDown();
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

        assert.commandWorked(PrepareHelpers.commitTransaction(newSession, prepareTimestamp));
        replTest.awaitReplication();

        postCommit(primary, newPrimary);

        jsTestLog("Running another transaction on the new primary");
        const secondSession = newPrimary.startSession({causalConsistency: false});
        secondSession.startTransaction({writeConcern: {w: "majority"}});
        assert.commandWorked(
            secondSession.getDatabase(dbName).getCollection(collName).insert({_id: "second-doc"}));
        assert.commandWorked(secondSession.commitTransaction_forTesting());

        // Unfreeze the original primary so that it can stand for election again for the next test.
        assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
    }

    function doInsert(primary, session) {
        const doc = {_id: "txn on primary " + primary};
        jsTestLog("Inserting a document in a transaction.");
        assert.commandWorked(session.getDatabase(dbName).getCollection(collName).insert(doc));
    }
    function postInsert(primary, newPrimary) {
        const doc = {_id: "txn on primary " + primary};
        assert.docEq(doc, primary.getDB(dbName).getCollection(collName).findOne());
        assert.docEq(doc, newPrimary.getDB(dbName).getCollection(collName).findOne());
    }

    function doInsertTextSearch(primary, session) {
        // Create an index outside of the transaction.
        assert.commandWorked(
            primary.getDB(dbName).getCollection(collName).createIndex({text: "text"}));

        // Do the followings in a transaction.
        jsTestLog("Inserting a document in a transaction.");
        assert.commandWorked(
            session.getDatabase(dbName).getCollection(collName).insert({text: "text"}));
        // Text search will recursively acquire the global lock. This tests that yielding
        // recursively held locks works on step down.
        jsTestLog("Doing a text search in a transaction.");
        assert.eq(1,
                  session.getDatabase(dbName)
                      .getCollection(collName)
                      .find({$text: {$search: "text"}})
                      .itcount());
    }
    function postInsertTextSearch(primary, newPrimary) {
        assert.eq(1,
                  primary.getDB(dbName)
                      .getCollection(collName)
                      .find({$text: {$search: "text"}})
                      .itcount());
        assert.eq(1,
                  newPrimary.getDB(dbName)
                      .getCollection(collName)
                      .find({$text: {$search: "text"}})
                      .itcount());
    }

    function stepDownViaHeartbeat() {
        jsTestLog("Stepping down primary via heartbeat");
        replTest.stepUp(replTest.getSecondary());
    }

    function stepDownViaCommand() {
        jsTestLog("Stepping down primary via command");
        assert.commandWorked(replTest.getPrimary().adminCommand({replSetStepDown: 10}));
    }

    testTransactionsWithFailover(doInsert, stepDownViaHeartbeat, postInsert);
    testTransactionsWithFailover(doInsert, stepDownViaCommand, postInsert);

    testTransactionsWithFailover(doInsertTextSearch, stepDownViaHeartbeat, postInsertTextSearch);
    testTransactionsWithFailover(doInsertTextSearch, stepDownViaCommand, postInsertTextSearch);

    replTest.stopSet();
})();
