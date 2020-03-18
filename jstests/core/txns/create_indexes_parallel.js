/* Tests parallel transactions with createIndexes.
 *
 * @tags: [uses_transactions,
 *         # Creating collections inside multi-document transactions is supported only in v4.4
 *         # onwards.
 *         requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/create_index_txn_helpers.js");

let doParallelCreateIndexesTest = function(explicitCollectionCreate, multikeyIndex) {
    const dbName = "test";
    const collName = "create_new_collection";
    const distinctCollName = collName + "_second";
    const session = db.getMongo().getDB(dbName).getMongo().startSession();
    const secondSession = db.getMongo().getDB(dbName).getMongo().startSession();

    let sessionDB = session.getDatabase("test");
    let secondSessionDB = secondSession.getDatabase("test");
    let sessionColl = sessionDB[collName];
    let secondSessionColl = secondSessionDB[collName];
    sessionColl.drop({writeConcern: {w: "majority"}});
    secondSessionColl.drop({writeConcern: {w: "majority"}});
    let distinctSessionColl = sessionDB[distinctCollName];
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing duplicate sequential createIndexes, both succeed");

    session.startTransaction({writeConcern: {w: "majority"}});        // txn 1
    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2

    createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
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

    createIndexAndCRUDInTxn(secondSessionDB, collName, explicitCollectionCreate, multikeyIndex);
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

    createIndexAndCRUDInTxn(sessionDB, distinctCollName, explicitCollectionCreate, multikeyIndex);
    createIndexAndCRUDInTxn(secondSessionDB, collName, explicitCollectionCreate, multikeyIndex);
    jsTest.log("Committing transaction 2");
    secondSession.commitTransaction();
    assert.eq(secondSessionColl.find({}).itcount(), 1);
    assert.eq(secondSessionColl.getIndexes().length, 2);

    // createIndexes takes minimum visible snapshots of new collections into consideration when
    // checking for existing indexes.
    assert.commandFailedWithCode(
        sessionColl.runCommand({createIndexes: collName, indexes: [conflictingIndexSpecs]}),
        ErrorCodes.SnapshotUnavailable);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(sessionColl.getIndexes().length, 2);
    assert.eq(distinctSessionColl.find({}).itcount(), 0);
    assert.eq(distinctSessionColl.getIndexes().length, 0);

    distinctSessionColl.drop({writeConcern: {w: "majority"}});
    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing duplicate createIndexes in parallel, both attempt to commit, second to commit fails");

    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    createIndexAndCRUDInTxn(secondSessionDB, collName, explicitCollectionCreate, multikeyIndex);

    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);

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
    createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
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
    createIndexAndCRUDInTxn(secondSessionDB, collName, explicitCollectionCreate, multikeyIndex);

    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);

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
    createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);

    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    createIndexAndCRUDInTxn(
        secondSessionDB, distinctCollName, explicitCollectionCreate, multikeyIndex);

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
