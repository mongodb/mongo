/* Tests simple cases of creating a collection inside a multi-document transaction, both
 * committing and aborting.
 *
 * @tags: [uses_transactions,
 *         # Creating collections inside multi-document transactions is supported only in v4.4
 *         # onwards.
 *         requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/create_collection_txn_helpers.js");
load("jstests/libs/fixture_helpers.js");  // for isMongos
load("jstests/libs/auto_retry_transaction_in_sharding.js");

function runCollectionCreateTest(command, explicitCreate) {
    const session = db.getMongo().startSession();
    const collName = "create_new_collection";
    const secondCollName = collName + "_second";

    let sessionDB = session.getDatabase("test");
    let sessionColl = sessionDB[collName];
    let secondSessionColl = sessionDB[secondCollName];
    sessionColl.drop({writeConcern: {w: "majority"}});
    secondSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing createCollection in a transaction");
    withTxnAndAutoRetryOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});
    assert.eq(sessionColl.find({}).itcount(), 1);

    jsTest.log("Testing createCollection in a transaction, implicitly creating database");
    assert.commandWorked(sessionDB.dropDatabase());
    withTxnAndAutoRetryOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});
    assert.eq(sessionColl.find({}).itcount(), 1);

    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing multiple createCollections in a transaction");
    withTxnAndAutoRetryOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
        createCollAndCRUDInTxn(sessionDB, secondCollName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});
    assert.eq(sessionColl.find({}).itcount(), 1);
    assert.eq(secondSessionColl.find({}).itcount(), 1);

    sessionColl.drop({writeConcern: {w: "majority"}});
    secondSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing createCollection in a transaction that aborts");
    session.startTransaction({writeConcern: {w: "majority"}});
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});
    assert.commandWorked(session.abortTransaction_forTesting());

    assert.eq(sessionColl.find({}).itcount(), 0);

    jsTest.log("Testing multiple createCollections in a transaction that aborts");
    session.startTransaction({writeConcern: {w: "majority"}});
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
        createCollAndCRUDInTxn(sessionDB, secondCollName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});
    session.abortTransaction();
    assert.eq(sessionColl.find({}).itcount(), 0);
    assert.eq(secondSessionColl.find({}).itcount(), 0);

    sessionColl.drop({writeConcern: {w: "majority"}});
    secondSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing createCollection on an existing collection in a transaction (SHOULD ABORT)");
    assert.commandWorked(sessionDB.runCommand({create: collName, writeConcern: {w: "majority"}}));
    session.startTransaction({writeConcern: {w: "majority"}});
    retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
        createCollAndCRUDInTxn(sessionDB, secondCollName, command, explicitCreate);
    }, {writeConcern: {w: "majority"}});

    assert.commandFailedWithCode(sessionDB.runCommand({create: collName}),
                                 ErrorCodes.NamespaceExists);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    assert.eq(sessionColl.find({}).itcount(), 0);
    assert.eq(secondSessionColl.find({}).itcount(), 0);

    sessionColl.drop({writeConcern: {w: "majority"}});

    // mongos does not support throwWCEDuringTxnCollCreate
    if (!FixtureHelpers.isMongos(db)) {
        jsTest.log(
            "Testing createCollection with writeConflict errors in a transaction (SHOULD ABORT");
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "throwWCEDuringTxnCollCreate", mode: "alwaysOn"}));
        session.startTransaction({writeConcern: {w: "majority"}});
        assertCollCreateFailedWithCode(
            sessionDB, collName, command, explicitCreate, ErrorCodes.WriteConflict);
        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
        assert.eq(sessionColl.find({}).itcount(), 0);
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "throwWCEDuringTxnCollCreate", mode: "off"}));
    }

    session.endSession();
}

runCollectionCreateTest("insert", true /*explicitCreate*/);
runCollectionCreateTest("insert", false /*explicitCreate*/);
runCollectionCreateTest("update", true /*explicitCreate*/);
runCollectionCreateTest("update", false /*explicitCreate*/);
runCollectionCreateTest("findAndModify", true /*explicitCreate*/);
runCollectionCreateTest("findAndModify", false /*explicitCreate*/);
}());
