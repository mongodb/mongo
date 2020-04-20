/* Tests simple cases of creating indexes inside a multi-document transaction, both
 * committing and aborting.
 *
 * @tags: [uses_transactions,
 *         # Creating collections inside multi-document transactions is supported only in v4.4
 *         # onwards.
 *         requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/auto_retry_transaction_in_sharding.js");
load("jstests/libs/create_index_txn_helpers.js");

let doCreateIndexesTest = function(explicitCollectionCreate, multikeyIndex) {
    const session = db.getMongo().startSession();
    const collName = "create_new_indexes";
    const secondCollName = collName + "_second";

    let sessionDB = session.getDatabase("test");
    let sessionColl = sessionDB[collName];
    let secondSessionColl = sessionDB[secondCollName];
    sessionColl.drop({writeConcern: {w: "majority"}});
    secondSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing createIndexes in a transaction");
    withTxnAndAutoRetryOnMongos(session, function() {
        createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});
    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(sessionColl.getIndexes().length, 2);
    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing multiple createIndexess in a transaction");
    withTxnAndAutoRetryOnMongos(session, function() {
        createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
        createIndexAndCRUDInTxn(sessionDB, secondCollName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});
    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(secondSessionColl.find({}).itcount(), 1);
    assert.eq(sessionColl.getIndexes().length, 2);
    assert.eq(secondSessionColl.getIndexes().length, 2);

    sessionColl.drop({writeConcern: {w: "majority"}});
    secondSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing createIndexes in a transaction that aborts");
    session.startTransaction({writeConcern: {w: "majority"}});
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});
    session.abortTransaction();

    assert.eq(sessionColl.find({}).itcount(), 0);
    assert.eq(sessionColl.getIndexes().length, 0);

    jsTest.log("Testing multiple createIndexes in a transaction that aborts");
    session.startTransaction({writeConcern: {w: "majority"}});
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
        createIndexAndCRUDInTxn(sessionDB, secondCollName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});
    session.abortTransaction();
    assert.eq(sessionColl.find({}).itcount(), 0);
    assert.eq(sessionColl.getIndexes().length, 0);
    assert.eq(secondSessionColl.find({}).itcount(), 0);
    assert.eq(secondSessionColl.getIndexes().length, 0);

    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing createIndexes with conflicting index specs in a transaction that aborts (SHOULD FAIL)");
    session.startTransaction({writeConcern: {w: "majority"}});
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createIndexAndCRUDInTxn(sessionDB, collName, explicitCollectionCreate, multikeyIndex);
    }, {writeConcern: {w: "majority"}});
    assert.commandFailedWithCode(
        sessionColl.runCommand({createIndexes: collName, indexes: [conflictingIndexSpecs]}),
        ErrorCodes.IndexKeySpecsConflict);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    assert.eq(sessionColl.find({}).itcount(), 0);
    assert.eq(sessionColl.getIndexes().length, 0);

    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing createIndexes on a non-empty collection created in the same transaction (SHOULD FAIL)");
    session.startTransaction({writeConcern: {w: "majority"}});
    assert.commandWorked(sessionDB.runCommand({create: collName}));
    assert.commandWorked(sessionColl.insert({a: 1}));

    assert.commandFailedWithCode(
        sessionColl.runCommand({createIndexes: collName, indexes: [indexSpecs]}),
        ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    assert.eq(sessionColl.find({}).itcount(), 0);
    assert.eq(sessionColl.getIndexes().length, 0);

    session.endSession();
};

doCreateIndexesTest(false /*explicitCollectionCreate*/, false /*multikeyIndex*/);
doCreateIndexesTest(true /*explicitCollectionCreate*/, false /*multikeyIndex*/);
doCreateIndexesTest(false /*explicitCollectionCreate*/, true /*multikeyIndex*/);
doCreateIndexesTest(true /*explicitCollectionCreate*/, true /*multikeyIndex*/);
}());
