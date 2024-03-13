/**
 * Tests parallel transactions with createCollections.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: endSession.
 *   not_allowed_with_signed_security_token,
 *   uses_transactions,
 * ]
 */
import {
    retryOnceOnTransientAndRestartTxnOnMongos
} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {createCollAndCRUDInTxn} from "jstests/libs/create_collection_txn_helpers.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const dbName = 'test_txns_create_collection_parallel';

function runParallelCollectionCreateTest(command, explicitCreate) {
    const dbName = 'test_txns_create_collection_parallel';
    const collName = "create_new_collection";
    const distinctCollName = collName + "_second";
    const session = db.getMongo().getDB(dbName).getMongo().startSession();
    const secondSession = db.getMongo().getDB(dbName).getMongo().startSession();

    let sessionDB = session.getDatabase(dbName);
    let secondSessionDB = secondSession.getDatabase(dbName);
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

    // create cannot observe the collection created in the other transaction so the command will
    // succeed and we will instead throw WCE when trying to commit the transaction.
    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        assert.commandWorked(secondSessionDB.runCommand({create: collName}));
    }, {writeConcern: {w: "majority"}});

    assert.commandFailedWithCode(secondSession.commitTransaction_forTesting(),
                                 ErrorCodes.WriteConflict);

    assert.eq(distinctSessionColl.find({}).itcount(), 0);
    sessionColl.drop({writeConcern: {w: "majority"}});
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    // TODO SERVER-77915 Remove isTrackUnshardedUponCreationDisabled. Once track unsharded is
    // enabled, creation within a transaction will always serialize with any other collection
    // creation by taking the DDLLock
    const isTrackUnshardedUponCreationDisabled = !FeatureFlagUtil.isPresentAndEnabled(
        db.getSiblingDB('admin'), "TrackUnshardedCollectionsUponCreation");
    if (isTrackUnshardedUponCreationDisabled) {
        jsTest.log("Testing duplicate createCollections, one inside and one outside a txn");
        session.startTransaction({writeConcern: {w: "majority"}});
        retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
            createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
        }, {writeConcern: {w: "majority"}});
        assert.commandWorked(secondSessionDB.runCommand({create: collName}));  // outside txn
        assert.commandWorked(secondSessionDB.getCollection(collName).insert({a: 1}));

        jsTest.log("Committing transaction (SHOULD FAIL)");
        assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                     ErrorCodes.WriteConflict);
        assert.eq(sessionColl.find({}).itcount(), 1);
    }

    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing duplicate createCollections in parallel, both attempt to commit, second to commit fails");

    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    retryOnceOnTransientAndRestartTxnOnMongos(secondSession, () => {
        createCollAndCRUDInTxn(
            secondSession.getDatabase(dbName), collName, command, explicitCreate);
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
            secondSession.getDatabase(dbName), collName, command, explicitCreate);
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
            secondSession.getDatabase(dbName), collName, command, explicitCreate);
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
