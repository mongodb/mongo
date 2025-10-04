/**
 * Tests parallel transactions with createCollections.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: endSession.
 *   not_allowed_with_signed_security_token,
 *   uses_transactions,
 * ]
 */
import {withRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {createCollAndCRUDInTxn} from "jstests/libs/txns/create_collection_txn_helpers.js";

const dbName = "test_txns_create_collection_parallel";

function runParallelCollectionCreateTest(command, explicitCreate) {
    const dbName = "test_txns_create_collection_parallel";
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
    withRetryOnTransientTxnError(
        () => {
            session.startTransaction({writeConcern: {w: "majority"}}); // txn 1
            secondSession.startTransaction({writeConcern: {w: "majority"}}); // txn 2
            createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
            jsTest.log("Committing transaction 1");
            session.commitTransaction();
            assert.eq(sessionColl.find({}).itcount(), 1);

            assert.commandFailedWithCode(secondSessionDB.runCommand({create: collName}), ErrorCodes.NamespaceExists);

            assert.commandFailedWithCode(secondSession.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
        },
        () => {
            session.abortTransaction();
            secondSession.abortTransaction();
            sessionColl.drop({writeConcern: {w: "majority"}});
        },
    );

    sessionColl.drop({writeConcern: {w: "majority"}});
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing duplicate createCollections, where failing createCollection performs a " +
            "successful operation earlier in the transaction.",
    );

    withRetryOnTransientTxnError(
        () => {
            session.startTransaction({writeConcern: {w: "majority"}}); // txn 1
            secondSession.startTransaction({writeConcern: {w: "majority"}}); // txn 2
            createCollAndCRUDInTxn(secondSessionDB, distinctCollName, command, explicitCreate);
            createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
            jsTest.log("Committing transaction 1");
            session.commitTransaction();
            assert.eq(sessionColl.find({}).itcount(), 1);

            // create cannot observe the collection created in the other transaction so the command
            // will succeed and we will instead throw WCE when trying to commit the transaction.
            assert.commandWorked(secondSessionDB.runCommand({create: collName}));

            assert.commandFailedWithCode(secondSession.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
        },
        () => {
            session.abortTransaction();
            secondSession.abortTransaction();
            sessionColl.drop({writeConcern: {w: "majority"}});
            distinctSessionColl.drop({writeConcern: {w: "majority"}});
        },
    );

    assert.eq(distinctSessionColl.find({}).itcount(), 0);
    sessionColl.drop({writeConcern: {w: "majority"}});
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    // Prevent this test case in case of implicit tracking of collections. The DDL lock taken by the
    // transaction won't be release until commit. This will cause the creation outside of a
    // transaction (which will attempt to take the ddl lock as well) to wait indefinitely.
    if (!TestData.implicitlyTrackUnshardedCollectionOnCreation) {
        jsTest.log("Testing duplicate createCollections, one inside and one outside a txn");

        withRetryOnTransientTxnError(
            () => {
                session.startTransaction({writeConcern: {w: "majority"}});
                createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);
                assert.commandWorked(secondSessionDB.runCommand({create: collName})); // outside txn
                assert.commandWorked(secondSessionDB.getCollection(collName).insert({a: 1}));

                jsTest.log("Committing transaction (SHOULD FAIL)");
                assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
                assert.eq(sessionColl.find({}).itcount(), 1);
            },
            () => {
                session.abortTransaction();
                sessionColl.drop({writeConcern: {w: "majority"}});
            },
        );
    }

    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing duplicate createCollections in parallel, both attempt to commit, second to commit fails");

    withRetryOnTransientTxnError(
        () => {
            secondSession.startTransaction({writeConcern: {w: "majority"}}); // txn 2
            createCollAndCRUDInTxn(secondSession.getDatabase(dbName), collName, command, explicitCreate);

            session.startTransaction({writeConcern: {w: "majority"}}); // txn 1
            createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);

            jsTest.log("Committing transaction 2");
            secondSession.commitTransaction();
            jsTest.log("Committing transaction 1 (SHOULD FAIL)");
            assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
            assert.eq(sessionColl.find({}).itcount(), 1);
        },
        () => {
            try {
                secondSession.abortTransaction();
                session.abortTransaction();
            } catch (e) {
                // ignore
            }
            sessionColl.drop({writeConcern: {w: "majority"}});
        },
    );

    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing duplicate createCollections which implicitly create databases in parallel" +
            ", both attempt to commit, second to commit fails",
    );

    assert.commandWorked(sessionDB.dropDatabase());
    withRetryOnTransientTxnError(
        () => {
            secondSession.startTransaction({writeConcern: {w: "majority"}}); // txn 2
            createCollAndCRUDInTxn(secondSession.getDatabase(dbName), collName, command, explicitCreate);

            session.startTransaction({writeConcern: {w: "majority"}}); // txn 1
            createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);

            jsTest.log("Committing transaction 2");
            secondSession.commitTransaction();
            jsTest.log("Committing transaction 1 (SHOULD FAIL)");
            assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
            assert.eq(sessionColl.find({}).itcount(), 1);
        },
        () => {
            try {
                secondSession.abortTransaction();
                session.abortTransaction();
            } catch (e) {
                // ignore
            }
            sessionDB.dropDatabase();
        },
    );
    sessionColl.drop({writeConcern: {w: "majority"}});
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing createCollection conflict during commit, where the conflict rolls back a " +
            "previously committed collection.",
    );

    withRetryOnTransientTxnError(
        () => {
            secondSession.startTransaction({writeConcern: {w: "majority"}}); // txn 2
            createCollAndCRUDInTxn(secondSession.getDatabase(dbName), collName, command, explicitCreate);

            session.startTransaction({writeConcern: {w: "majority"}}); // txn 1
            createCollAndCRUDInTxn(sessionDB, distinctCollName, command, explicitCreate); // does not conflict
            createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate); // conflicts

            jsTest.log("Committing transaction 2");
            secondSession.commitTransaction();
            jsTest.log("Committing transaction 1 (SHOULD FAIL)");
            assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
            assert.eq(sessionColl.find({}).itcount(), 1);
            assert.eq(distinctSessionColl.find({}).itcount(), 0);
        },
        () => {
            try {
                secondSession.abortTransaction();
                session.abortTransaction();
            } catch (e) {
                // ignore
            }
            sessionColl.drop({writeConcern: {w: "majority"}});
            distinctSessionColl.drop({writeConcern: {w: "majority"}});
        },
    );

    sessionColl.drop({writeConcern: {w: "majority"}});
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing distinct createCollections in parallel, both successfully commit.");
    withRetryOnTransientTxnError(
        () => {
            session.startTransaction({writeConcern: {w: "majority"}}); // txn 1
            createCollAndCRUDInTxn(sessionDB, collName, command, explicitCreate);

            secondSession.startTransaction({writeConcern: {w: "majority"}}); // txn 2
            createCollAndCRUDInTxn(secondSessionDB, distinctCollName, command, explicitCreate);

            session.commitTransaction();
            secondSession.commitTransaction();
        },
        () => {
            try {
                session.abortTransaction();
                secondSession.abortTransaction();
            } catch (e) {
                // ignore
            }
            sessionColl.drop({writeConcern: {w: "majority"}});
            distinctSessionColl.drop({writeConcern: {w: "majority"}});
        },
    );

    secondSession.endSession();
    session.endSession();
}
runParallelCollectionCreateTest("insert", true /*explicitCreate*/);
runParallelCollectionCreateTest("insert", false /*explicitCreate*/);
runParallelCollectionCreateTest("update", true /*explicitCreate*/);
runParallelCollectionCreateTest("update", false /*explicitCreate*/);
runParallelCollectionCreateTest("findAndModify", true /*explicitCreate*/);
runParallelCollectionCreateTest("findAndModify", false /*explicitCreate*/);
