/**
 * Tests parallel transactions with createIndexes.
 *
 * The test runs commands that are not allowed with security token: endSession.
 * @tags: [
 *   not_allowed_with_security_token,
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/auto_retry_transaction_in_sharding.js");
load("jstests/libs/create_index_txn_helpers.js");
load("jstests/libs/feature_flag_util.js");

let doParallelCreateIndexesTest = function(explicitCollectionCreate, multikeyIndex) {
    const dbName = 'test_txns_create_indexes_parallel';
    const collName = "create_new_collection";
    const distinctCollName = collName + "_second";
    const session = db.getMongo().getDB(dbName).getMongo().startSession();
    const secondSession = db.getMongo().getDB(dbName).getMongo().startSession();

    let sessionDB = session.getDatabase(dbName);
    let secondSessionDB = secondSession.getDatabase(dbName);
    let sessionColl = sessionDB[collName];
    let secondSessionColl = secondSessionDB[collName];
    sessionColl.drop({writeConcern: {w: "majority"}});
    secondSessionColl.drop({writeConcern: {w: "majority"}});
    let distinctSessionColl = sessionDB[distinctCollName];
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing duplicate sequential createIndexes, both succeed");
    session.startTransaction({writeConcern: {w: "majority"}});        // txn 1
    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2

    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});
    jsTest.log("Committing transaction 1");
    session.commitTransaction();
    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(sessionColl.getIndexes().length, 2);

    // Ensuring existing index succeeds.
    assert.commandWorked(
        secondSessionColl.runCommand({createIndexes: collName, indexes: [indexSpecs]}));
    secondSession.commitTransaction();
    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(sessionColl.getIndexes().length, 2);

    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing conflicting sequential createIndexes, second fails");
    session.startTransaction({writeConcern: {w: "majority"}});        // txn 1
    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2

    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        createIndexAndCRUDInTxn(secondSessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});
    jsTest.log("Committing transaction 2");
    secondSession.commitTransaction();
    assert.eq(secondSessionColl.find({}).itcount(), 1);
    assert.eq(secondSessionColl.getIndexes().length, 2);

    assert.commandFailedWithCode(
        sessionColl.runCommand({createIndexes: collName, indexes: [conflictingIndexSpecs]}),
        ErrorCodes.IndexKeySpecsConflict);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(sessionColl.getIndexes().length, 2);

    distinctSessionColl.drop({writeConcern: {w: "majority"}});
    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing conflicting sequential createIndexes, where failing createIndexes " +
               "performs a successful index creation earlier in the transaction.");
    session.startTransaction({writeConcern: {w: "majority"}});        // txn 1
    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2

    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createIndexAndCRUDInTxn(
            sessionDB, distinctCollName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});

    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        createIndexAndCRUDInTxn(secondSessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});
    jsTest.log("Committing transaction 2");
    secondSession.commitTransaction();
    assert.eq(secondSessionColl.find({}).itcount(), 1);
    assert.eq(secondSessionColl.getIndexes().length, 2);

    // createIndexes cannot observe the index created in the other transaction so the command will
    // succeed and we will instead throw WCE when trying to commit the transaction.
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        assert.commandWorked(
            sessionColl.runCommand({createIndexes: collName, indexes: [conflictingIndexSpecs]}));
    }, {writeConcern: {w: "majority"}});

    assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);

    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(sessionColl.getIndexes().length, 2);
    assert.eq(distinctSessionColl.find({}).itcount(), 0);
    assert.eq(distinctSessionColl.getIndexes().length, 0);

    distinctSessionColl.drop({writeConcern: {w: "majority"}});
    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing duplicate createIndexes in parallel, both attempt to commit, second to commit fails");
    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        createIndexAndCRUDInTxn(secondSessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});

    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});

    jsTest.log("Committing transaction 2");
    secondSession.commitTransaction();
    jsTest.log("Committing transaction 1 (SHOULD FAIL)");
    // WriteConflict occurs here because in all test cases (i.e., explicitCollectionCreate is true
    // versus false), we must create a collection as part of each transaction. The conflicting
    // collection creation causes the WriteConflict.
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(sessionColl.getIndexes().length, 2);

    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing createIndexes inside txn and createCollection on conflicting collection " +
               "in parallel.");
    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});
    assert.commandWorked(secondSessionDB.createCollection(collName));
    assert.commandWorked(secondSessionDB.getCollection(collName).insert({a: 1}));

    jsTest.log("Committing transaction (SHOULD FAIL)");
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(sessionColl.getIndexes().length, 1);

    assert.commandWorked(sessionDB.dropDatabase());
    jsTest.log("Testing duplicate createIndexes which implicitly create a database in parallel" +
               ", both attempt to commit, second to commit fails");
    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        createIndexAndCRUDInTxn(secondSessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});

    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});

    jsTest.log("Committing transaction 2");
    secondSession.commitTransaction();

    jsTest.log("Committing transaction 1 (SHOULD FAIL)");
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(sessionColl.getIndexes().length, 2);

    sessionColl.drop({writeConcern: {w: "majority"}});
    secondSessionColl.drop({writeConcern: {w: "majority"}});
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing distinct createIndexes in parallel, both successfully commit.");
    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});

    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        createIndexAndCRUDInTxn(
            secondSessionDB, distinctCollName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});

    session.commitTransaction();
    secondSession.commitTransaction();

    secondSession.endSession();
    session.endSession();
};

doParallelCreateIndexesTest(false /*explicitCollectionCreate*/, false /*multikeyIndex*/);
doParallelCreateIndexesTest(true /*explicitCollectionCreate*/, false /*multikeyIndex*/);
doParallelCreateIndexesTest(false /*explicitCollectionCreate*/, true /*multikeyIndex*/);
doParallelCreateIndexesTest(true /*explicitCollectionCreate*/, true /*multikeyIndex*/);
}());
