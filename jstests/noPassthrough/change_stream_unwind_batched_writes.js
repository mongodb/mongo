/**
 * Verifies change streams operation for batched writes.
 *
 * @tags: [
 *   requires_fcv_61,
 *   requires_replication,
 *   requires_majority_read_concern,
 *   uses_change_streams,
 * ]
 */
(function() {
"use strict";

const dbName = "test";
const collName = "c";

/**
 * Asserts that the expected operation type and documentKey are found on the change stream
 * cursor. Returns the change stream document.
 */
function assertWriteVisible(cursor, operationType, documentKey) {
    assert.soon(() => cursor.hasNext());
    const changeDoc = cursor.next();
    assert.eq(operationType, changeDoc.operationType, changeDoc);
    assert.eq(documentKey, changeDoc.documentKey, changeDoc);
    // Change stream events for batched writes do not include lsid and txnNumber.
    assert(!changeDoc.hasOwnProperty('lsid'));
    assert(!changeDoc.hasOwnProperty('txnNumber'));
    return changeDoc;
}

/**
 * Asserts that the expected operation type and documentKey are found on the change stream
 * cursor. Pushes the corresponding resume token and change stream document to an array.
 */
function assertWriteVisibleWithCapture(cursor, operationType, documentKey, changeList) {
    const changeDoc = assertWriteVisible(cursor, operationType, documentKey);
    changeList.push(changeDoc);
}

/**
 * Asserts that there are no changes waiting on the change stream cursor.
 */
function assertNoChanges(cursor) {
    assert(!cursor.hasNext(), () => {
        return "Unexpected change set: " + tojson(cursor.toArray());
    });
}

function runTest(conn) {
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);

    const docsPerBatch = 3;
    const totalNumDocs = 8;
    let changeList = [];

    // For consistent results, disable any batch targeting except for
    // 'batchedDeletesTargetBatchDocs'.
    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetBatchTimeMS: 0}));
    assert.commandWorked(db.adminCommand({setParameter: 1, batchedDeletesTargetStagedDocBytes: 0}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, batchedDeletesTargetBatchDocs: docsPerBatch}));

    // Populate the collection, then open a change stream, then mass-delete the collection.
    assert.commandWorked(
        coll.insertMany([...Array(totalNumDocs).keys()].map(x => ({_id: x, txt: "a" + x}))));
    const changeStreamCursor = coll.watch();
    const serverStatusBatchesBefore = db.serverStatus()['batchedDeletes']['batches'];
    const serverStatusDocsBefore = db.serverStatus()['batchedDeletes']['docs'];
    assert.commandWorked(coll.deleteMany({_id: {$gte: 0}}));
    assert.eq(0, coll.find().itcount());
    const serverStatusBatchesAfter = db.serverStatus()['batchedDeletes']['batches'];
    const serverStatusDocsAfter = db.serverStatus()['batchedDeletes']['docs'];
    assert.eq(serverStatusBatchesAfter,
              serverStatusBatchesBefore + Math.ceil(totalNumDocs / docsPerBatch));
    assert.eq(serverStatusDocsAfter, serverStatusDocsBefore + totalNumDocs);

    // Verify the change stream emits events for the batched deletion, and capture the events so we
    // can test resumability later.
    for (let docKey = 0; docKey < totalNumDocs; docKey++) {
        assertWriteVisibleWithCapture(changeStreamCursor, "delete", {_id: docKey}, changeList);
    }

    assertNoChanges(changeStreamCursor);
    changeStreamCursor.close();

    // Test that change stream resume returns the expected set of documents at each point
    // captured by this test.
    for (let i = 0; i < changeList.length; ++i) {
        const resumeCursor = coll.watch([], {startAfter: changeList[i]._id});

        for (let x = (i + 1); x < changeList.length; ++x) {
            const expectedChangeDoc = changeList[x];
            assertWriteVisible(
                resumeCursor, expectedChangeDoc.operationType, expectedChangeDoc.documentKey);
        }

        assertNoChanges(resumeCursor);
        resumeCursor.close();
    }

    assert.commandWorked(db.dropDatabase());
}

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

runTest(rst.getPrimary());

rst.stopSet();
})();
