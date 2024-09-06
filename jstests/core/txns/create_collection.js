/**
 * Tests simple cases of creating a collection inside a multi-document transaction, both
 * committing and aborting.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: endSession.
 *   not_allowed_with_signed_security_token,
 *   uses_transactions,
 * ]
 */
import {
    withAbortAndRetryOnTransientTxnError,
    withTxnAndAutoRetryOnMongos
} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {
    assertCollCreateFailedWithCode,
    createCollAndCRUDInTxn
} from "jstests/libs/create_collection_txn_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

function runCollectionCreateTest(command, explicitCreate) {
    const session = db.getMongo().startSession();
    const dbName = 'test_txns_create_collection';
    const collName = "create_new_collection";
    // Note: using strange collection name here to test sorting of operations by namespace,
    // SERVER-48628
    const secondCollName = "\n" + collName + "_second";

    let sessionDB = session.getDatabase(dbName);
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
    withAbortAndRetryOnTransientTxnError(session, () => {
        session.startTransaction({writeConcern: {w: "majority"}});
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
    });
    assert.commandWorked(session.abortTransaction_forTesting());

    assert.eq(sessionColl.find({}).itcount(), 0);

    jsTest.log("Testing multiple createCollections in a transaction that aborts");
    withAbortAndRetryOnTransientTxnError(session, () => {
        session.startTransaction({writeConcern: {w: "majority"}});
        createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
        createCollAndCRUDInTxn(sessionDB, secondCollName, command, explicitCreate);
    });
    session.abortTransaction();
    assert.eq(sessionColl.find({}).itcount(), 0);
    assert.eq(secondSessionColl.find({}).itcount(), 0);

    sessionColl.drop({writeConcern: {w: "majority"}});
    secondSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing createCollection on an existing collection in a transaction (SHOULD ABORT)");
    assert.commandWorked(sessionDB.runCommand({create: collName, writeConcern: {w: "majority"}}));
    withAbortAndRetryOnTransientTxnError(session, () => {
        session.startTransaction({writeConcern: {w: "majority"}});
        createCollAndCRUDInTxn(sessionDB, secondCollName, command, explicitCreate);
    });

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
