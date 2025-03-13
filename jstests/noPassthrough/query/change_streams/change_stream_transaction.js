/**
 * Confirms that change streams only see committed operations for prepared transactions.
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {assertNoChanges} from "jstests/libs/query/change_stream_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isTimestamp} from "jstests/libs/timestamp_util.js";

const dbName = "test";
const collName = "change_stream_transaction";

/**
 * This test sets an internal parameter in order to force transactions with more than 4
 * operations to span multiple oplog entries, making it easier to test that scenario.
 */
const maxOpsInOplogEntry = 4;

/**
 * Asserts that the expected operation type and documentKey are found on the change stream
 * cursor. Returns the change stream document.
 */
function assertWriteVisible(cursor, operationType, documentKey) {
    assert.soon(() => cursor.hasNext());
    const changeDoc = cursor.next();
    assert.eq(operationType, changeDoc.operationType, changeDoc);
    if (operationType !== 'endOfTransaction') {
        assert.eq(documentKey, changeDoc.documentKey, changeDoc);
    }
    return changeDoc;
}

/**
 * Asserts that the expected operation type and documentKey are found on the change stream
 * cursor. Pushes the corresponding resume token and change stream document to an array.
 */
function assertWriteVisibleWithCapture(
    cursor, operationType, documentKey, changeList, expectedCommitTimestamp = null) {
    const changeDoc = assertWriteVisible(cursor, operationType, documentKey);
    if (expectedCommitTimestamp !== null) {
        assert(changeDoc.hasOwnProperty("commitTimestamp"), changeDoc);
        assert.eq(changeDoc["commitTimestamp"], expectedCommitTimestamp, changeDoc);
    } else {
        assert(!changeDoc.hasOwnProperty("commitTimestamp"), changeDoc);
    }
    changeList.push(changeDoc);
}

function runTest(conn) {
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);
    const unwatchedColl = db.getCollection(collName + "_unwatched");

    const fcvDoc = db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    const is81OrHigher =
        (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, "8.1") >= 0);

    // This function will return null for all versions < 8.1, because these won't emit the
    // "commitTimestamp" as part of the events of prepared transactions.
    const buildCommitTimestamp = (prepareTimestamp) => {
        assert(isTimestamp(prepareTimestamp), prepareTimestamp);
        if (is81OrHigher) {
            return prepareTimestamp;
        }
        return null;
    };

    let changeList = [];

    // Collections must be created outside of any transaction.
    assert.commandWorked(db.createCollection(coll.getName()));
    assert.commandWorked(db.createCollection(unwatchedColl.getName()));

    //
    // Start transaction 1.
    //
    const session1 = db.getMongo().startSession();
    const sessionDb1 = session1.getDatabase(dbName);
    const sessionColl1 = sessionDb1[collName];
    session1.startTransaction({readConcern: {level: "majority"}});

    //
    // Start transaction 2.
    //
    const session2 = db.getMongo().startSession();
    const sessionDb2 = session2.getDatabase(dbName);
    const sessionColl2 = sessionDb2[collName];
    session2.startTransaction({readConcern: {level: "majority"}});

    //
    // Start transaction 3.
    //
    const session3 = db.getMongo().startSession();
    const sessionDb3 = session3.getDatabase(dbName);
    const sessionColl3 = sessionDb3[collName];
    session3.startTransaction({readConcern: {level: "majority"}});

    // Open a change stream on the test collection.
    const changeStreamCursor = coll.watch([], {showExpandedEvents: true});
    const resumeToken = changeStreamCursor.getResumeToken();

    // Insert a document and confirm that the change stream has it.
    assert.commandWorked(coll.insert({_id: "no-txn-doc-1"}, {writeConcern: {w: "majority"}}));
    assertWriteVisibleWithCapture(changeStreamCursor, "insert", {_id: "no-txn-doc-1"}, changeList);

    // Insert two documents under each transaction and confirm no change stream updates.
    assert.commandWorked(sessionColl1.insert([{_id: "txn1-doc-1"}, {_id: "txn1-doc-2"}]));
    assert.commandWorked(sessionColl2.insert([{_id: "txn2-doc-1"}, {_id: "txn2-doc-2"}]));
    // No changes are expected from the above operations. However, we are deliberately not calling
    // 'assertNoChanges(changeStreamCursor)' here to avoid stalling the test for some seconds.
    // Instead, we execute some more commands that produce no changes and then call
    // 'assertNoChanges(changeStreamCursor)' for all of them at once, still expecting no changes for
    // any of the buffered operations.

    // Update one document under each transaction and confirm no change stream updates.
    assert.commandWorked(sessionColl1.update({_id: "txn1-doc-1"}, {$set: {"updated": 1}}));
    assert.commandWorked(sessionColl2.update({_id: "txn2-doc-1"}, {$set: {"updated": 1}}));

    // Update and then remove the second doc under each transaction and confirm no change stream
    // events are seen.
    assert.commandWorked(
        sessionColl1.update({_id: "txn1-doc-2"}, {$set: {"update-before-delete": 1}}));
    assert.commandWorked(
        sessionColl2.update({_id: "txn2-doc-2"}, {$set: {"update-before-delete": 1}}));
    assert.commandWorked(sessionColl1.remove({_id: "txn1-doc-2"}));
    assert.commandWorked(sessionColl2.remove({_id: "txn2-doc-2"}));

    // Perform a write to the 'session1' transaction in a collection that is not being watched
    // by 'changeStreamCursor'. We do not expect to see this write in the change stream either
    // now or on commit.
    assert.commandWorked(
        sessionDb1[unwatchedColl.getName()].insert({_id: "txn1-doc-unwatched-collection"}));

    // Perform a write to the 'session3' transaction in a collection that is not being watched
    // by 'changeStreamCursor'. We do not expect to see this write in the change stream either
    // now or on commit.
    assert.commandWorked(
        sessionDb3[unwatchedColl.getName()].insert({_id: "txn3-doc-unwatched-collection"}));
    assertNoChanges(changeStreamCursor);

    // Perform a write outside of a transaction and confirm that the change stream sees only
    // this write.
    assert.commandWorked(coll.insert({_id: "no-txn-doc-2"}, {writeConcern: {w: "majority"}}));
    assertWriteVisibleWithCapture(changeStreamCursor, "insert", {_id: "no-txn-doc-2"}, changeList);

    let prepareTimestampTxn1 = PrepareHelpers.prepareTransaction(session1);

    assert.commandWorked(coll.insert({_id: "no-txn-doc-3"}, {writeConcern: {w: "majority"}}));
    assertWriteVisibleWithCapture(changeStreamCursor, "insert", {_id: "no-txn-doc-3"}, changeList);

    //
    // Commit first transaction and confirm expected changes.
    //
    assert.commandWorked(PrepareHelpers.commitTransaction(session1, prepareTimestampTxn1));
    [["insert", {_id: "txn1-doc-1"}],
     ["insert", {_id: "txn1-doc-2"}],
     ["update", {_id: "txn1-doc-1"}],
     ["update", {_id: "txn1-doc-2"}],
     ["delete", {_id: "txn1-doc-2"}],
    ].forEach(([opType, id]) => {
        assertWriteVisibleWithCapture(
            changeStreamCursor, opType, id, changeList, buildCommitTimestamp(prepareTimestampTxn1));
    });

    // Transition the second transaction to prepared. We skip capturing the prepare
    // timestamp it is not required for abortTransaction_forTesting().
    PrepareHelpers.prepareTransaction(session2);
    assertNoChanges(changeStreamCursor);

    assert.commandWorked(coll.insert({_id: "no-txn-doc-4"}, {writeConcern: {w: "majority"}}));
    assertWriteVisibleWithCapture(changeStreamCursor, "insert", {_id: "no-txn-doc-4"}, changeList);

    //
    // Abort second transaction.
    //
    session2.abortTransaction_forTesting();

    //
    // Start transaction 4.
    //
    const session4 = db.getMongo().startSession();
    const sessionDb4 = session4.getDatabase(dbName);
    const sessionColl4 = sessionDb4[collName];
    session4.startTransaction({readConcern: {level: "majority"}});

    // Perform enough writes to fill up one applyOps.
    const txn4Inserts = Array.from({length: maxOpsInOplogEntry},
                                   (_, index) => ({_id: {name: "txn4-doc", index: index}}));
    txn4Inserts.forEach(function(doc) {
        sessionColl4.insert(doc);
    });

    // Perform enough writes to an unwatched collection to fill up a second applyOps. We
    // specifically want to test the case where a multi-applyOps transaction has no relevant
    // updates in its final applyOps.
    txn4Inserts.forEach(function(doc) {
        assert.commandWorked(sessionDb4[unwatchedColl.getName()].insert(doc));
    });

    //
    // Start transaction 5.
    //
    const session5 = db.getMongo().startSession();
    const sessionDb5 = session5.getDatabase(dbName);
    const sessionColl5 = sessionDb5[collName];
    session5.startTransaction({readConcern: {level: "majority"}});

    // Perform enough writes to span 3 applyOps entries.
    const txn5Inserts = Array.from({length: 3 * maxOpsInOplogEntry},
                                   (_, index) => ({_id: {name: "txn5-doc", index: index}}));
    txn5Inserts.forEach(function(doc) {
        assert.commandWorked(sessionColl5.insert(doc));
    });

    //
    // Prepare and commit transaction 5.
    //
    const prepareTimestampTxn5 = PrepareHelpers.prepareTransaction(session5);
    assertNoChanges(changeStreamCursor);
    assert.commandWorked(PrepareHelpers.commitTransaction(session5, prepareTimestampTxn5));
    txn5Inserts.forEach(function(doc) {
        assertWriteVisibleWithCapture(changeStreamCursor,
                                      "insert",
                                      doc,
                                      changeList,
                                      buildCommitTimestamp(prepareTimestampTxn5));
    });

    //
    // Commit transaction 4 without preparing.
    //
    session4.commitTransaction();
    txn4Inserts.forEach(function(doc) {
        assertWriteVisibleWithCapture(changeStreamCursor, "insert", doc, changeList);
    });
    if (FeatureFlagUtil.isEnabled(db, "EndOfTransactionChangeEvent")) {
        assertWriteVisibleWithCapture(changeStreamCursor, "endOfTransaction", {}, changeList);
    } else {
        assertNoChanges(changeStreamCursor);
    }

    changeStreamCursor.close();

    // Test that the change stream returns the expected set of documents at each point captured by
    // this test, and not any additional events.
    const resumeCursor = coll.watch([], {startAfter: resumeToken, showExpandedEvents: true});
    for (let i = 0; i < changeList.length; ++i) {
        const expectedChangeDoc = changeList[i];
        const actualChangeDoc = assertWriteVisible(
            resumeCursor, expectedChangeDoc.operationType, expectedChangeDoc.documentKey);
        assert.eq(expectedChangeDoc._id, actualChangeDoc._id);
    }
    assertNoChanges(resumeCursor);
    resumeCursor.close();

    //
    // Prepare and commit the third transaction and confirm that there are no visible changes.
    //
    const prepareTimestampTxn3 = PrepareHelpers.prepareTransaction(session3);
    assertNoChanges(changeStreamCursor);

    assert.commandWorked(PrepareHelpers.commitTransaction(session3, prepareTimestampTxn3));
    assertNoChanges(changeStreamCursor);

    assert.commandWorked(db.dropDatabase());
}

let replSetTestDescription = {nodes: 1};
if (!jsTest.options().setParameters.hasOwnProperty(
        "maxNumberOfTransactionOperationsInSingleOplogEntry")) {
    // Configure the replica set to use our value for maxOpsInOplogEntry.
    replSetTestDescription.nodeOptions = {
        setParameter: {maxNumberOfTransactionOperationsInSingleOplogEntry: maxOpsInOplogEntry}
    };
} else {
    // The test is executing in a build variant that already defines its own override value for
    // maxNumberOfTransactionOperationsInSingleOplogEntry. Even though the build variant's
    // choice for this override won't test the same edge cases, the test should still succeed.
}
const rst = new ReplSetTest(replSetTestDescription);
rst.startSet();
rst.initiate();

runTest(rst.getPrimary());

rst.stopSet();
