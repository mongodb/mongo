/**
 * Tests the updateOne command on time-series collections in multi-document transactions.
 *
 * @tags: [
 *   requires_replication,
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

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
};

// 1. Update one document from the collection in a transaction.
(function basicUpdateOne() {
    jsTestLog("Running 'basicUpdateOne'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());

    session.startTransaction();
    assert.commandWorked(sessionColl.updateOne({_id: 0, [metaFieldName]: 0}, {$inc: {updated: 1}}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // Expect one updated document with updated: 1.
    assert.eq(testColl.find({updated: 1}).toArray().length, 1);
})();

// 2. updateOne should not have visible changes when the transaction is aborted.
(function updateOneTransactionAborts() {
    jsTestLog("Running 'updateOneTransactionAborts'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());

    session.startTransaction();
    assert.commandWorked(sessionColl.updateOne({_id: 0, [metaFieldName]: 1}, {$inc: {updated: 1}}));
    assert.commandWorked(session.abortTransaction_forTesting());
    session.endSession();

    // The transaction was aborted so no documents should have been updated.
    assert.eq(testColl.find({updated: 1}).toArray().length, 0);
})();

// 3. Run a few updateOnes in a single transaction.
(function multipleUpdateOne() {
    jsTestLog("Running 'multipleUpdateOne'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());
    session.startTransaction();

    for (let i = 0; i < docsPerMetaField; ++i) {
        assert.commandWorked(
            sessionColl.updateOne({_id: i, [metaFieldName]: 0}, {$inc: {updated: 1}}));
    }

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // Expect all documents with {meta: 0} to be updated.
    assert.eq(testColl.find({[metaFieldName]: 0, updated: 1}).toArray().length, docsPerMetaField);
})();

// 4. Tests performing updateOnes in and out of a transaction on abort.
(function mixedUpdateOneAbortTxn() {
    jsTestLog("Running 'mixedUpdateOneAbortTxn'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());
    session.startTransaction();

    // Update all documents for meta values 0, 1.
    for (let i = 0; i < docsPerMetaField; ++i) {
        assert.commandWorked(
            sessionColl.updateOne({_id: i, [metaFieldName]: 0}, {$inc: {updated: 1}}));
        assert.commandWorked(
            sessionColl.updateOne({_id: i, [metaFieldName]: 1}, {$inc: {updated: 1}}));
    }

    // Outside of the session and transaction, perform an updateOne.
    assert.commandWorked(testColl.updateOne({_id: 0, [metaFieldName]: 2}, {$inc: {updated: 1}}));

    assert.commandWorked(session.abortTransaction_forTesting());
    session.endSession();

    // The aborted transaction should not have updated any documents.
    assert.eq(testColl.find({[metaFieldName]: 0, updated: 1}).toArray().length, 0);
    assert.eq(testColl.find({[metaFieldName]: 1, updated: 1}).toArray().length, 0);

    // The update outside of the transaction should have succeeded.
    assert.eq(testColl.find({[metaFieldName]: 2, updated: 1}).toArray().length, 1);
})();

// 5. Tests performing updateOnes in and out of a transaction on commit.
(function mixedUpdateOneCommitTxn() {
    jsTestLog("Running 'mixedUpdateOneCommitTxn'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());
    session.startTransaction();

    for (let i = 0; i < docsPerMetaField; ++i) {
        // Within the transaction.
        assert.commandWorked(
            sessionColl.updateOne({_id: i, [metaFieldName]: 0}, {$inc: {updated: 1}}));
        assert.commandWorked(
            sessionColl.updateOne({_id: i, [metaFieldName]: 1}, {$inc: {updated: 1}}));

        // Outside of the session and transaction, perform updateOne.
        assert.commandWorked(
            testColl.updateOne({_id: i, [metaFieldName]: 2}, {$inc: {updated: 1}}));
    }

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // Expect all documents to have been updated.
    assert.eq(testColl.find({updated: 1}).toArray().length, 9);
})();

// 6. Tests a race to update the same document in and out of a transaction.
(function raceToUpdateOne() {
    jsTestLog("Running 'raceToUpdateOne'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());
    session.startTransaction();

    // Within the transaction, perform an updateOne.
    const updateFilter = {_id: 1, [metaFieldName]: 0};
    assert.commandWorked(sessionColl.updateOne(updateFilter, {$set: {_id: 10}}));

    // Note: there is a change the parallel shell runs after the transaction is committed and that
    // is fine as both interleavings should succeed.
    const awaitTestUpdate = startParallelShell(
        funWithArgs(function(dbName, collName, filter) {
            const testDB = db.getSiblingDB(dbName);
            const coll = testDB.getCollection(collName);

            // Outside of the session and transaction, perform updateOne.
            assert.commandWorked(coll.updateOne(filter, {$set: {_id: 10}}));
        }, testDB.getName(), testColl.getName(), updateFilter), testDB.getMongo().port);

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // Allow non-transactional updateOne to finish.
    awaitTestUpdate();
    assert.eq(testColl.find({_id: 10}).toArray().length, 1);
})();

// 7. Tests a transactional updateOne on a document which gets visible after the transaction
// starts.
(function updateOneAndInsertBeforeCommit() {
    jsTestLog("Running 'updateOneAndInsertBeforeCommit'");
    initializeData();

    const session = testDB.getMongo().startSession();
    const sessionColl = session.getDatabase(jsTestName()).getCollection(testColl.getName());

    session.startTransaction();
    // Ensure the document does not exist within the snapshot of the newly started transaction.
    assert.eq(sessionColl.find({[metaFieldName]: 101}).toArray().length, 0);

    // Outside of the session and transaction, update document.
    assert.commandWorked(
        testColl.updateOne({[metaFieldName]: 0, _id: 0}, {$set: {[metaFieldName]: 101}}));

    // Double check the document is still not visible from within the transaction.
    assert.eq(sessionColl.find({[metaFieldName]: 101}).toArray().length, 0);

    // Within the transaction, perform updateOne.
    assert.commandWorked(sessionColl.updateOne({[metaFieldName]: 101}, {$inc: {updated: 1}}));
    assert.eq(sessionColl.find({updated: 1}).toArray().length, 0);

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // The newly updated document should not be updated even though the transaction commits after
    // the write performed outside.
    assert.eq(testColl.find({updated: 1}).toArray().length, 0);
})();

// 8. Tests two side-by-side transactional updateOnes on the same document.
(function updateOneInTwoTransactions() {
    jsTestLog("Running 'updateOneInTwoTransactions'");
    initializeData();

    const sessionA = testDB.getMongo().startSession();
    const sessionB = testDB.getMongo().startSession();
    const collA = sessionA.getDatabase(jsTestName()).getCollection(testColl.getName());
    const collB = sessionB.getDatabase(jsTestName()).getCollection(testColl.getName());

    const docToUpdate = {_id: 1, [metaFieldName]: 1};

    // Start transactions on different sessions.
    sessionA.startTransaction({readConcern: {level: "snapshot"}});
    sessionB.startTransaction({readConcern: {level: "snapshot"}});

    // Ensure the document exists in the snapshot of both transactions.
    assert.eq(collA.find(docToUpdate).toArray().length, 1);
    assert.eq(collB.find(docToUpdate).toArray().length, 1);

    // Perform updateOne on transaction A.
    assert.commandWorked(collA.updateOne(docToUpdate, {$inc: {updated: 1}}));

    const updateCommand = {
        update: collB.getName(),
        updates: [{
            q: docToUpdate,
            u: {$inc: {updated: 1}},
            multi: false,
        }]
    };

    // We expect the updateOne on transaction B to fail, causing the transaction to abort.
    // Sidenote: avoiding the updateOne method from 'crud_api.js' because it throws.
    assert.commandFailedWithCode(collB.runCommand(updateCommand), ErrorCodes.WriteConflict);
    assert.commandFailedWithCode(sessionB.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    sessionB.endSession();

    // Ensure the document is updated in the snapshot of transaction A.
    assert.eq(collA.find({updated: 1}).toArray().length, 1);
    // Since transaction A has not committed yet, the document should still not be updated outside
    // of the transaction.
    assert.eq(testColl.find({updated: 1}).toArray().length, 0);

    // Ensure the document has been successfully updated after transaction A commits.
    assert.commandWorked(sessionA.commitTransaction_forTesting());
    assert.eq(testColl.find({updated: 1}).toArray().length, 1);

    sessionA.endSession();
})();

rst.stopSet();