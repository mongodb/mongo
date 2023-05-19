/**
 * Tests the deleteOne command on time-series collections in multi-document transactions.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const metaFieldName = "mm";
const timeFieldName = "tt";
const collectionNamePrefix = "test_coll_";
let collectionCounter = 0;

const testDB = rst.getPrimary().getDB(jsTestName());
let testColl = testDB[collectionNamePrefix + collectionCounter];
assert.commandWorked(testDB.dropDatabase());

const docsPerMetaField = 3;
const initializeData = function() {
    testColl = testDB[collectionNamePrefix + ++collectionCounter];
    assert.commandWorked(testDB.createCollection(
        testColl.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    let docs = [];
    for (let i = 0; i < docsPerMetaField; ++i) {
        docs.push({_id: i, [timeFieldName]: new Date(), [metaFieldName]: 0});
        docs.push({_id: i, [timeFieldName]: new Date(), [metaFieldName]: 1});
        docs.push({_id: i, [timeFieldName]: new Date(), [metaFieldName]: 2});
    }

    // Insert test documents.
    assert.commandWorked(testColl.insertMany(docs));
    printjson("Printing docs: " + tojson(testColl.find({}).toArray()));
};

// 1. Delete one document from the collection in a transaction.
(function basicDeleteOne() {
    jsTestLog("Running 'basicDeleteOne'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());

    session.startTransaction();
    assert.commandWorked(sessionColl.deleteOne({_id: 0, [metaFieldName]: 0}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // Expect one deleted document with meta: 0.
    assert.eq(testColl.find({[metaFieldName]: 0}).toArray().length, docsPerMetaField - 1);
})();

// 2. deleteOne should not have visible changes when the transaction is aborted.
(function deleteOneTransactionAborts() {
    jsTestLog("Running 'deleteOneTransactionAborts'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());

    session.startTransaction();
    assert.commandWorked(sessionColl.deleteOne({_id: 0, [metaFieldName]: 1}));
    assert.commandWorked(session.abortTransaction_forTesting());
    session.endSession();

    // The transaction was aborted so no documents should have been deleted.
    assert.eq(testColl.find({[metaFieldName]: 1}).toArray().length, docsPerMetaField);
})();

// 3. Run a few deleteOnes in a single transaction.
(function multipleDeleteOne() {
    jsTestLog("Running 'multipleDeleteOne'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());
    session.startTransaction();

    for (let i = 0; i < docsPerMetaField; ++i) {
        assert.commandWorked(sessionColl.deleteOne({_id: i, [metaFieldName]: 0}));
    }

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // Expect all documents with {meta: 0} to be deleted.
    assert.eq(testColl.find({[metaFieldName]: 0}).toArray().length, 0);
})();

// 4. Tests performing deleteOnes in and out of a transaction on abort.
(function mixedDeleteOneAbortTxn() {
    jsTestLog("Running 'mixedDeleteOneAbortTxn'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());
    session.startTransaction();

    // Delete all documents for meta values 0, 1.
    for (let i = 0; i < docsPerMetaField; ++i) {
        assert.commandWorked(sessionColl.deleteOne({_id: i, [metaFieldName]: 0}));
        assert.commandWorked(sessionColl.deleteOne({_id: i, [metaFieldName]: 1}));
    }

    // Outside of the session and transaction, perform a deleteOne.
    const docFilterNoTxn = {_id: 0, [metaFieldName]: 2};
    assert.commandWorked(testColl.deleteOne(docFilterNoTxn));

    assert.commandWorked(session.abortTransaction_forTesting());
    session.endSession();

    // The aborted transaction should not have deleted any documents.
    assert.eq(testColl.find({[metaFieldName]: 0}).toArray().length, docsPerMetaField);
    assert.eq(testColl.find({[metaFieldName]: 1}).toArray().length, docsPerMetaField);

    // The delete outside of the transaction should have succeeded.
    assert.eq(testColl.find(docFilterNoTxn).toArray().length, 0);
})();

// 5. Tests performing deleteOnes in and out of a transaction on commit.
(function mixedDeleteOneCommitTxn() {
    jsTestLog("Running 'mixedDeleteOneCommitTxn'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());
    session.startTransaction();

    for (let i = 0; i < docsPerMetaField; ++i) {
        // Within the transaction.
        assert.commandWorked(sessionColl.deleteOne({_id: i, [metaFieldName]: 0}));
        assert.commandWorked(sessionColl.deleteOne({_id: i, [metaFieldName]: 1}));

        // Outside of the session and transaction, perform deleteOne.
        assert.commandWorked(testColl.deleteOne({_id: i, [metaFieldName]: 2}));
    }

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // Expect all documents to have been deleted.
    assert.eq(testColl.find({}).toArray().length, 0);
})();

// 6. Tests a race to delete the same document in and out of a transaction.
(function raceToDeleteOne() {
    jsTestLog("Running 'raceToDeleteOne'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());
    session.startTransaction();

    // Within the transaction, perform deleteOne.
    const deleteFilter = {_id: 1, [metaFieldName]: 0};
    assert.commandWorked(sessionColl.deleteOne(deleteFilter));

    // Note: there is a change the parallel shell runs after the transcation is committed and that
    // is fine as both interleavings should succeed.
    const awaitTestDelete = startParallelShell(
        funWithArgs(function(dbName, collName, filter) {
            const testDB = db.getSiblingDB(dbName);
            const coll = testDB.getCollection(collName);

            // Outside of the session and transaction, perform deleteOne.
            assert.commandWorked(coll.deleteOne(filter));
        }, testDB.getName(), testColl.getName(), deleteFilter), testDB.getMongo().port);

    assert.commandWorked(session.commitTransaction_forTesting());
    assert.eq(testColl.find(deleteFilter).toArray().length, 0);
    session.endSession();

    // Allow non-transactional deleteOne to finish.
    awaitTestDelete();
    assert.eq(testColl.find(deleteFilter).toArray().length, 0);
})();

// 7. Tests a transactional deleteOne on a document which gets inserted after the transaction
// starts.
(function deleteOneAndInsertBeforeCommit() {
    jsTestLog("Running 'deleteOneAndInsertBeforeCommit'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());
    const newDoc = {_id: 101, [timeFieldName]: new Date(), [metaFieldName]: 101};

    session.startTransaction();
    // Ensure the document does not exist within the snapshot of the newly started transaction.
    assert.eq(sessionColl.find(newDoc).toArray().length, 0);

    // Outside of the session and transaction, insert document.
    assert.commandWorked(testColl.insert(newDoc));

    // Double check the document is still not visible from within the transaction.
    assert.eq(sessionColl.find(newDoc).toArray().length, 0);

    // Within the transaction, perform deleteOne.
    assert.commandWorked(sessionColl.deleteOne(newDoc));
    assert.eq(sessionColl.find(newDoc).toArray().length, 0);

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // The newly inserted document should be present even though the transaction commits after the
    // insert.
    assert.eq(testColl.find(newDoc).toArray().length, 1);
})();

// 8. Tests two side-by-side transactional deleteOnes on the same document.
(function deleteOneInTwoTransactions() {
    jsTestLog("Running 'deleteOneInTwoTransactions'");
    initializeData();

    const sessionA = testDB.getMongo().startSession();
    const sessionB = testDB.getMongo().startSession();
    const collA = sessionA.getDatabase(jsTestName()).getCollection(testColl.getName());
    const collB = sessionB.getDatabase(jsTestName()).getCollection(testColl.getName());

    const docToDelete = {_id: 1, [metaFieldName]: 1};

    // Start transactions on different sessions.
    sessionA.startTransaction({readConcern: {level: "snapshot"}});
    sessionB.startTransaction({readConcern: {level: "snapshot"}});

    // Ensure the document exists in the snapshot of both transactions.
    assert.eq(collA.find(docToDelete).toArray().length, 1);
    assert.eq(collB.find(docToDelete).toArray().length, 1);

    // Perform deleteOne on transaction A.
    assert.commandWorked(collA.deleteOne(docToDelete));

    const deleteCommand = {
        delete: collB.getName(),
        deletes: [{
            q: docToDelete,
            limit: 1,
        }]
    };

    // We expect the deleteOne on transaction B to fail, causing the transaction to abort.
    // Sidenote: avoiding the deleteOne method from 'crud_api.js' because it throws.
    assert.commandFailedWithCode(collB.runCommand(deleteCommand), ErrorCodes.WriteConflict);
    assert.commandFailedWithCode(sessionB.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    sessionB.endSession();

    // Ensure the document does not exist in the snapshot of transaction A.
    assert.eq(collA.find(docToDelete).toArray().length, 0);
    // Since transaction A has not committed yet, the document should still be present outside of
    // the transaction.
    assert.eq(testColl.find(docToDelete).toArray().length, 1);

    // Ensure the document has been successfully deleted after transaction A commits.
    assert.commandWorked(sessionA.commitTransaction_forTesting());
    assert.eq(testColl.find(docToDelete).toArray().length, 0);

    sessionA.endSession();
})();

rst.stopSet();
})();
