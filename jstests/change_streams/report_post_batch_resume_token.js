/**
 * Tests that an aggregate with a $changeStream stage reports the latest postBatchResumeToken.
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    // Drop and recreate collections to assure a clean run.
    const collName = "report_post_batch_resume_token";
    const testCollection = assertDropAndRecreateCollection(db, collName);
    const otherCollection = assertDropAndRecreateCollection(db, "unrelated_" + collName);
    const adminDB = db.getSiblingDB("admin");

    let docId = 0;  // Tracks _id of documents inserted to ensure that we do not duplicate.
    const batchSize = 2;

    // Test that postBatchResumeToken is present on an initial aggregate of batchSize: 0.
    let csCursor = testCollection.watch([], {cursor: {batchSize: 0}});
    assert.eq(csCursor.objsLeftInBatch(), 0);
    let initialAggPBRT = csCursor.getResumeToken();
    assert.neq(undefined, initialAggPBRT);

    // Test that the PBRT does not advance beyond its initial value for a change stream whose
    // startAtOperationTime is in the future, even as writes are made to the test collection.
    const timestampIn2100 = Timestamp(4102444800, 1);
    csCursor = testCollection.watch([], {startAtOperationTime: timestampIn2100});
    assert.eq(csCursor.objsLeftInBatch(), 0);
    initialAggPBRT = csCursor.getResumeToken();
    assert.neq(undefined, initialAggPBRT);

    // Write some documents to the test collection.
    for (let i = 0; i < 5; ++i) {
        assert.commandWorked(testCollection.insert({_id: docId++}));
    }

    // Verify that no events are returned and the PBRT does not advance or go backwards.
    assert(!csCursor.hasNext());  // Causes a getMore to be dispatched.
    let getMorePBRT = csCursor.getResumeToken();
    assert.eq(bsonWoCompare(initialAggPBRT, getMorePBRT), 0);

    // Test that postBatchResumeToken is present on empty initial aggregate batch.
    csCursor = testCollection.watch();
    assert.eq(csCursor.objsLeftInBatch(), 0);
    initialAggPBRT = csCursor.getResumeToken();
    assert.neq(undefined, initialAggPBRT);

    // Test that postBatchResumeToken is present on empty getMore batch.
    assert(!csCursor.hasNext());  // Causes a getMore to be dispatched.
    getMorePBRT = csCursor.getResumeToken();
    assert.neq(undefined, getMorePBRT);
    assert.gte(bsonWoCompare(getMorePBRT, initialAggPBRT), 0);

    // Test that postBatchResumeToken advances with returned events. Insert one document into the
    // collection and consume the resulting change stream event.
    assert.commandWorked(testCollection.insert({_id: docId++}));
    assert(csCursor.hasNext());  // Causes a getMore to be dispatched.
    assert(csCursor.objsLeftInBatch() == 1);

    // Because the retrieved event is the most recent entry in the oplog, the PBRT should be equal
    // to the resume token of the last item in the batch and greater than the initial PBRT.
    let resumeTokenFromDoc = csCursor.next()._id;
    getMorePBRT = csCursor.getResumeToken();
    assert.eq(bsonWoCompare(getMorePBRT, resumeTokenFromDoc), 0);
    assert.gt(bsonWoCompare(getMorePBRT, initialAggPBRT), 0);

    // Now seed the collection with enough documents to fit in two batches.
    for (let i = 0; i < batchSize * 2; i++) {
        assert.commandWorked(testCollection.insert({_id: docId++}));
    }

    // Test that the PBRT for a resumed stream is the given resume token if no result are returned.
    csCursor = testCollection.watch([], {resumeAfter: resumeTokenFromDoc, cursor: {batchSize: 0}});
    assert.eq(csCursor.objsLeftInBatch(), 0);
    initialAggPBRT = csCursor.getResumeToken();
    assert.neq(undefined, initialAggPBRT);
    assert.eq(bsonWoCompare(initialAggPBRT, resumeTokenFromDoc), 0);

    // Test that postBatchResumeToken is present on non-empty initial aggregate batch.
    csCursor =
        testCollection.watch([], {resumeAfter: resumeTokenFromDoc, cursor: {batchSize: batchSize}});
    assert.eq(csCursor.objsLeftInBatch(), batchSize);
    while (csCursor.objsLeftInBatch()) {
        csCursor.next();
    }
    // We see a postBatchResumeToken on the initial aggregate command. Because we resumed after the
    // previous getMorePBRT, the postBatchResumeToken from this stream compares greater than it.
    initialAggPBRT = csCursor.getResumeToken();
    assert.neq(undefined, initialAggPBRT);
    assert.gt(bsonWoCompare(initialAggPBRT, getMorePBRT), 0);

    // Test that postBatchResumeToken advances with getMore. Iterate the cursor and assert that the
    // observed postBatchResumeToken advanced.
    assert(csCursor.hasNext());  // Causes a getMore to be dispatched.
    assert.eq(csCursor.objsLeftInBatch(), batchSize);

    // The postBatchResumeToken is again equal to the final token in the batch, and greater than the
    // PBRT from the initial response.
    while (csCursor.objsLeftInBatch()) {
        resumeTokenFromDoc = csCursor.next()._id;
    }
    getMorePBRT = csCursor.getResumeToken();
    assert.eq(bsonWoCompare(resumeTokenFromDoc, getMorePBRT), 0);
    assert.gt(bsonWoCompare(getMorePBRT, initialAggPBRT), 0);

    // Test that postBatchResumeToken advances with writes to an unrelated collection. First make
    // sure there is nothing left in our cursor, and obtain the latest PBRT...
    assert(!csCursor.hasNext());  // Causes a getMore to be dispatched.
    let previousGetMorePBRT = csCursor.getResumeToken();
    assert.neq(undefined, previousGetMorePBRT);

    // ... then test that it advances on an insert to an unrelated collection.
    assert.commandWorked(otherCollection.insert({}));
    assert(!csCursor.hasNext());  // Causes a getMore to be dispatched.
    getMorePBRT = csCursor.getResumeToken();
    assert.gt(bsonWoCompare(getMorePBRT, previousGetMorePBRT), 0);

    // Insert two documents into the collection which are of the maximum BSON object size.
    const bsonUserSizeLimit = assert.commandWorked(adminDB.isMaster()).maxBsonObjectSize;
    assert.gt(bsonUserSizeLimit, 0);
    for (let i = 0; i < 2; ++i) {
        const docToInsert = {_id: docId++, padding: ""};
        docToInsert.padding = "a".repeat(bsonUserSizeLimit - Object.bsonsize(docToInsert));
        assert.commandWorked(testCollection.insert(docToInsert));
    }

    // Test that we return the correct postBatchResumeToken in the event that the batch hits the
    // byte size limit. Despite the fact that the batchSize is 2, we should only see 1 result,
    // because the second result cannot fit in the batch.
    assert(csCursor.hasNext());  // Causes a getMore to be dispatched.
    assert.eq(csCursor.objsLeftInBatch(), 1);

    // Verify that the postBatchResumeToken matches the last event actually added to the batch.
    resumeTokenFromDoc = csCursor.next()._id;
    getMorePBRT = csCursor.getResumeToken();
    assert.eq(bsonWoCompare(getMorePBRT, resumeTokenFromDoc), 0);

    // Now retrieve the second event and confirm that the PBRT matches its resume token.
    previousGetMorePBRT = getMorePBRT;
    assert(csCursor.hasNext());  // Causes a getMore to be dispatched.
    assert.eq(csCursor.objsLeftInBatch(), 1);
    resumeTokenFromDoc = csCursor.next()._id;
    getMorePBRT = csCursor.getResumeToken();
    assert.gt(bsonWoCompare(getMorePBRT, previousGetMorePBRT), 0);
    assert.eq(bsonWoCompare(getMorePBRT, resumeTokenFromDoc), 0);

    // Test that the PBRT is correctly updated when reading events from within a transaction.
    const session = db.getMongo().startSession();
    const sessionDB = session.getDatabase(db.getName());

    const sessionColl = sessionDB[testCollection.getName()];
    const sessionOtherColl = sessionDB[otherCollection.getName()];
    session.startTransaction();

    // Write 3 documents to testCollection and 1 to the unrelated collection within the transaction.
    for (let i = 0; i < 3; ++i) {
        assert.commandWorked(sessionColl.insert({_id: docId++}));
    }
    assert.commandWorked(sessionOtherColl.insert({}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // Grab the next 2 events, which should be the first 2 events in the transaction.
    previousGetMorePBRT = getMorePBRT;
    assert(csCursor.hasNext());  // Causes a getMore to be dispatched.
    assert.eq(csCursor.objsLeftInBatch(), 2);

    // The clusterTime should be the same on each, but the resume token keeps advancing.
    const txnEvent1 = csCursor.next(), txnEvent2 = csCursor.next();
    const txnClusterTime = txnEvent1.clusterTime;
    assert.eq(txnEvent2.clusterTime, txnClusterTime);
    assert.gt(bsonWoCompare(txnEvent1._id, previousGetMorePBRT), 0);
    assert.gt(bsonWoCompare(txnEvent2._id, txnEvent1._id), 0);

    // The PBRT of the first transaction batch is equal to the last document's resumeToken.
    getMorePBRT = csCursor.getResumeToken();
    assert.eq(bsonWoCompare(getMorePBRT, txnEvent2._id), 0);

    // Now get the next batch. This contains the third of the four transaction operations.
    previousGetMorePBRT = getMorePBRT;
    assert(csCursor.hasNext());  // Causes a getMore to be dispatched.
    assert.eq(csCursor.objsLeftInBatch(), 1);

    // The clusterTime of this event is the same as the two events from the previous batch, but its
    // resume token is greater than the previous PBRT.
    const txnEvent3 = csCursor.next();
    assert.eq(txnEvent3.clusterTime, txnClusterTime);
    assert.gt(bsonWoCompare(txnEvent3._id, previousGetMorePBRT), 0);

    // Because we wrote to the unrelated collection, the final event in the transaction does not
    // appear in the batch. But in this case it also does not allow our PBRT to advance beyond the
    // last event in the batch, because the unrelated event is within the same transaction and
    // therefore has the same clusterTime.
    getMorePBRT = csCursor.getResumeToken();
    assert.eq(bsonWoCompare(getMorePBRT, txnEvent3._id), 0);
})();
