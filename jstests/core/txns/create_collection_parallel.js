/* Tests parallel transactions with createCollections.
 *
 * @tags: [uses_transactions,
 *         # Creating collections inside multi-document transactions is supported only in v4.4
 *         # onwards.
 *         requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/create_collection_txn_helpers.js");
load("jstests/libs/auto_retry_transaction_in_sharding.js");

function runParallelCollectionCreateTest(command, explicitCreate) {
    const dbName = "test";
    const collName = "create_new_collection";
    const distinctCollName = collName + "_second";
    const session = db.getMongo().getDB(dbName).getMongo().startSession();
    const secondSession = db.getMongo().getDB(dbName).getMongo().startSession();

    let sessionDB = session.getDatabase("test");
    let secondSessionDB = secondSession.getDatabase("test");
    let sessionColl = sessionDB[collName];
    sessionColl.drop({writeConcern: {w: "majority"}});
    let distinctSessionColl = sessionDB[distinctCollName];
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing duplicate createCollections, second createCollection fails");

    session.startTransaction({writeConcern: {w: "majority"}});        // txn 1
    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});
    jsTest.log("Committing transaction 1");
    session.commitTransaction();
    assert.eq(sessionColl.find({}).itcount(), 1);

    assert.commandFailedWithCode(secondSessionDB.runCommand({create: collName}),
                                 ErrorCodes.NamespaceExists);

    assert.commandFailedWithCode(secondSession.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    sessionColl.drop({writeConcern: {w: "majority"}});
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing duplicate createCollections, where failing createCollection performs a " +
               "successful operation earlier in the transaction.");

    session.startTransaction({writeConcern: {w: "majority"}});        // txn 1
    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2

    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        createCollAndCRUDInTxn(secondSessionDB, distinctCollName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});
    jsTest.log("Committing transaction 1");
    session.commitTransaction();
    assert.eq(sessionColl.find({}).itcount(), 1);

    assert.commandFailedWithCode(secondSessionDB.runCommand({create: collName}),
                                 ErrorCodes.NamespaceExists);

    assert.commandFailedWithCode(secondSession.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    assert.eq(distinctSessionColl.find({}).itcount(), 0);
    sessionColl.drop({writeConcern: {w: "majority"}});
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing duplicate createCollections, one inside and one outside a txn");
    session.startTransaction({writeConcern: {w: "majority"}});
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});
    assert.commandWorked(secondSessionDB.runCommand({create: collName}));  // outside txn
    assert.commandWorked(secondSessionDB.getCollection(collName).insert({a: 1}));

    jsTest.log("Committing transaction (SHOULD FAIL)");
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
    assert.eq(sessionColl.find({}).itcount(), 1);

    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing duplicate createCollections in parallel, both attempt to commit, second to commit fails");

    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        createCollAndCRUDInTxn(
            secondSession.getDatabase("test"), collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});

    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});

    jsTest.log("Committing transaction 2");
    secondSession.commitTransaction();
    jsTest.log("Committing transaction 1 (SHOULD FAIL)");
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
    assert.eq(sessionColl.find({}).itcount(), 1);

    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing duplicate createCollections which implicitly create databases in parallel" +
               ", both attempt to commit, second to commit fails");

    assert.commandWorked(sessionDB.dropDatabase());
    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        createCollAndCRUDInTxn(
            secondSession.getDatabase("test"), collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});

    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});

    jsTest.log("Committing transaction 2");
    secondSession.commitTransaction();
    jsTest.log("Committing transaction 1 (SHOULD FAIL)");
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
    assert.eq(sessionColl.find({}).itcount(), 1);
    sessionColl.drop({writeConcern: {w: "majority"}});
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing createCollection conflict during commit, where the conflict rolls back a " +
               "previously committed collection.");

    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        createCollAndCRUDInTxn(
            secondSession.getDatabase("test"), collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});

    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createCollAndCRUDInTxn(
            sessionDB, distinctCollName, command, explicitCreate);             // does not conflict
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);  // conflicts
    }, {writeConcern: {w: "majority"}});

    jsTest.log("Committing transaction 2");
    secondSession.commitTransaction();
    jsTest.log("Committing transaction 1 (SHOULD FAIL)");
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(distinctSessionColl.find({}).itcount(), 0);

    sessionColl.drop({writeConcern: {w: "majority"}});
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing distinct createCollections in parallel, both successfully commit.");
    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});

    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        createCollAndCRUDInTxn(secondSessionDB, distinctCollName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});

    session.commitTransaction();
    secondSession.commitTransaction();

    secondSession.endSession();
    session.endSession();
}
runParallelCollectionCreateTest("insert", true /*explicitCreate*/);
runParallelCollectionCreateTest("insert", false /*explicitCreate*/);
runParallelCollectionCreateTest("update", true /*explicitCreate*/);
runParallelCollectionCreateTest("update", false /*explicitCreate*/);
runParallelCollectionCreateTest("findAndModify", true /*explicitCreate*/);
runParallelCollectionCreateTest("findAndModify", false /*explicitCreate*/);
}());
