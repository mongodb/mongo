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

    // Helper function to return the next batch given an initial aggregate command response.
    function runNextGetMore(initialCursorResponse) {
        const getMoreCollName = initialCursorResponse.cursor.ns.substr(
            initialCursorResponse.cursor.ns.indexOf('.') + 1);
        return assert.commandWorked(testCollection.runCommand({
            getMore: initialCursorResponse.cursor.id,
            collection: getMoreCollName,
            batchSize: batchSize
        }));
    }

    let docId = 0;  // Tracks _id of documents inserted to ensure that we do not duplicate.
    const batchSize = 2;

    // Test that postBatchResumeToken is present on empty initial aggregate batch.
    let initialAggResponse = assert.commandWorked(testCollection.runCommand(
        {aggregate: collName, pipeline: [{$changeStream: {}}], cursor: {batchSize: batchSize}}));

    // Examine the response from the initial agg. It should have a postBatchResumeToken (PBRT),
    // despite the fact that the batch is empty.
    let initialAggPBRT = initialAggResponse.cursor.postBatchResumeToken;
    assert.neq(undefined, initialAggPBRT, tojson(initialAggResponse));
    assert.eq(0, initialAggResponse.cursor.firstBatch.length);

    // Test that postBatchResumeToken is present on empty getMore batch.
    let getMoreResponse = runNextGetMore(initialAggResponse);
    let getMorePBRT = getMoreResponse.cursor.postBatchResumeToken;
    assert.neq(undefined, getMorePBRT, tojson(getMoreResponse));
    assert.gte(bsonWoCompare(getMorePBRT, initialAggPBRT), 0);
    assert.eq(0, getMoreResponse.cursor.nextBatch.length);

    // Test that postBatchResumeToken advances with returned events. Insert one document into the
    // collection and consume the resulting change stream event.
    assert.commandWorked(testCollection.insert({_id: docId++}));
    getMoreResponse = runNextGetMore(initialAggResponse);
    assert.eq(1, getMoreResponse.cursor.nextBatch.length);

    // Because the retrieved event is the most recent entry in the oplog, the PBRT should be equal
    // to the resume token of the last item in the batch and greater than the initial PBRT.
    let resumeTokenFromDoc = getMoreResponse.cursor.nextBatch[0]._id;
    getMorePBRT = getMoreResponse.cursor.postBatchResumeToken;
    assert.docEq(getMorePBRT, resumeTokenFromDoc);
    assert.gt(bsonWoCompare(getMorePBRT, initialAggPBRT), 0);

    // Now seed the collection with enough documents to fit in two batches.
    for (let i = 0; i < batchSize * 2; i++) {
        assert.commandWorked(testCollection.insert({_id: docId++}));
    }

    // Test that postBatchResumeToken is present on non-empty initial aggregate batch.
    initialAggResponse = assert.commandWorked(testCollection.runCommand({
        aggregate: collName,
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromDoc}}],
        cursor: {batchSize: batchSize}
    }));
    // We see a postBatchResumeToken on the initial aggregate command. Because we resumed after the
    // previous getMorePBRT, the postBatchResumeToken from this stream compares greater than it.
    initialAggPBRT = initialAggResponse.cursor.postBatchResumeToken;
    assert.neq(undefined, initialAggPBRT, tojson(initialAggResponse));
    assert.eq(batchSize, initialAggResponse.cursor.firstBatch.length);
    assert.gt(bsonWoCompare(initialAggPBRT, getMorePBRT), 0);

    // Test that postBatchResumeToken advances with getMore. Iterate the cursor and assert that the
    // observed postBatchResumeToken advanced.
    getMoreResponse = runNextGetMore(initialAggResponse);
    assert.eq(batchSize, getMoreResponse.cursor.nextBatch.length);

    // The postBatchResumeToken is again equal to the final token in the batch, and greater than the
    // PBRT from the initial response.
    resumeTokenFromDoc = getMoreResponse.cursor.nextBatch[batchSize - 1]._id;
    getMorePBRT = getMoreResponse.cursor.postBatchResumeToken;
    assert.docEq(resumeTokenFromDoc, getMorePBRT, tojson(getMoreResponse));
    assert.gt(bsonWoCompare(getMorePBRT, initialAggPBRT), 0);

    // Test that postBatchResumeToken advances with writes to an unrelated collection. First make
    // sure there is nothing left in our cursor, and obtain the latest PBRT...
    getMoreResponse = runNextGetMore(initialAggResponse);
    let previousGetMorePBRT = getMoreResponse.cursor.postBatchResumeToken;
    assert.neq(undefined, previousGetMorePBRT, tojson(getMoreResponse));
    assert.eq(getMoreResponse.cursor.nextBatch, []);

    // ... then test that it advances on an insert to an unrelated collection.
    assert.commandWorked(otherCollection.insert({}));
    getMoreResponse = runNextGetMore(initialAggResponse);
    assert.eq(0, getMoreResponse.cursor.nextBatch.length);
    getMorePBRT = getMoreResponse.cursor.postBatchResumeToken;
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
    getMoreResponse = runNextGetMore(initialAggResponse);
    assert.eq(1, getMoreResponse.cursor.nextBatch.length);

    // Verify that the postBatchResumeToken matches the last event actually added to the batch.
    resumeTokenFromDoc = getMoreResponse.cursor.nextBatch[0]._id;
    getMorePBRT = getMoreResponse.cursor.postBatchResumeToken;
    assert.docEq(getMorePBRT, resumeTokenFromDoc);

    // Now retrieve the second event and confirm that the PBRT matches its resume token.
    previousGetMorePBRT = getMorePBRT;
    getMoreResponse = runNextGetMore(initialAggResponse);
    resumeTokenFromDoc = getMoreResponse.cursor.nextBatch[0]._id;
    getMorePBRT = getMoreResponse.cursor.postBatchResumeToken;
    assert.eq(1, getMoreResponse.cursor.nextBatch.length);
    assert.gt(bsonWoCompare(getMorePBRT, previousGetMorePBRT), 0);
    assert.docEq(getMorePBRT, resumeTokenFromDoc);

    // Test that the PBRT is correctly updated when reading events from within a transaction.
    const session = db.getMongo().startSession();
    const sessionDB = session.getDatabase(db.getName());

    const sessionColl = sessionDB[testCollection.getName()];
    const sessionOtherColl = sessionDB[otherCollection.getName()];
    session.startTransaction();

    // Write 3 documents to the test collection and 1 to the unrelated collection.
    for (let i = 0; i < 3; ++i) {
        assert.commandWorked(sessionColl.insert({_id: docId++}));
    }
    assert.commandWorked(sessionOtherColl.insert({_id: docId++}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();

    // Grab the next 2 events, which should be the first 2 events in the transaction.
    previousGetMorePBRT = getMorePBRT;
    getMoreResponse = runNextGetMore(initialAggResponse);
    assert.eq(2, getMoreResponse.cursor.nextBatch.length);

    // The clusterTime should be the same on each, but the resume token keeps advancing.
    const txnEvent1 = getMoreResponse.cursor.nextBatch[0],
          txnEvent2 = getMoreResponse.cursor.nextBatch[1];
    const txnClusterTime = txnEvent1.clusterTime;
    assert.eq(txnEvent2.clusterTime, txnClusterTime);
    assert.gt(bsonWoCompare(txnEvent1._id, previousGetMorePBRT), 0);
    assert.gt(bsonWoCompare(txnEvent2._id, txnEvent1._id), 0);

    // The PBRT of the first transaction batch is equal to the last document's resumeToken.
    getMorePBRT = getMoreResponse.cursor.postBatchResumeToken;
    assert.docEq(getMorePBRT, txnEvent2._id);

    // Now get the next batch. This contains the third and final transaction operation.
    previousGetMorePBRT = getMorePBRT;
    getMoreResponse = runNextGetMore(initialAggResponse);
    assert.eq(1, getMoreResponse.cursor.nextBatch.length);

    // The clusterTime of this event is the same as the two events from the previous batch, but its
    // resume token is greater than the previous PBRT.
    const txnEvent3 = getMoreResponse.cursor.nextBatch[0];
    assert.eq(txnEvent3.clusterTime, txnClusterTime);
    assert.gt(bsonWoCompare(txnEvent3._id, previousGetMorePBRT), 0);

    // Because we wrote to the unrelated collection, the final event in the transaction does not
    // appear in the batch. But in this case it also does not allow our PBRT to advance beyond the
    // last event in the batch, because the unrelated event is within the same transaction and
    // therefore has the same clusterTime.
    getMorePBRT = getMoreResponse.cursor.postBatchResumeToken;
    assert.docEq(getMorePBRT, txnEvent3._id);
})();
