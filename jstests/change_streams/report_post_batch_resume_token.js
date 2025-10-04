/**
 * Tests that an aggregate with a $changeStream stage reports the latest postBatchResumeToken. This
 * test verifies postBatchResumeToken semantics that are common to sharded and unsharded streams.
 * @tags: [uses_transactions]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// Drop and recreate collections to assure a clean run.
const collName = "report_post_batch_resume_token";
const testCollection = assertDropAndRecreateCollection(db, collName);
const otherCollection = assertDropAndRecreateCollection(db, "unrelated_" + collName);
const adminDB = db.getSiblingDB("admin");

// Helper function which swallows an assertion if we are running on a sharded cluster.
assert.eqIfNotMongos = function (val1, val2, errMsg) {
    if (!FixtureHelpers.isMongos(db)) {
        assert.eq(val1, val2, errMsg);
    }
};

let docId = 0; // Tracks _id of documents inserted to ensure that we do not duplicate.
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

// Verify that no events are returned and the PBRT does not advance or go backwards, even as
// documents are written into the collection.
for (let i = 0; i < 5; ++i) {
    assert(!csCursor.hasNext()); // Causes a getMore to be dispatched.
    const getMorePBRT = csCursor.getResumeToken();
    assert.eq(bsonWoCompare(initialAggPBRT, getMorePBRT), 0);
    assert.commandWorked(testCollection.insert({_id: docId++}));
}

// Test that postBatchResumeToken is present on empty initial aggregate batch.
csCursor = testCollection.watch();
assert.eq(csCursor.objsLeftInBatch(), 0);
initialAggPBRT = csCursor.getResumeToken();
assert.neq(undefined, initialAggPBRT);

// Test that postBatchResumeToken is present on empty getMore batch.
assert(!csCursor.hasNext()); // Causes a getMore to be dispatched.
let getMorePBRT = csCursor.getResumeToken();
assert.neq(undefined, getMorePBRT);
assert.gte(bsonWoCompare(getMorePBRT, initialAggPBRT), 0);

// Test that postBatchResumeToken advances with returned events. Insert one document into the
// collection and consume the resulting change stream event.
assert.commandWorked(testCollection.insert({_id: docId++}));
assert.soon(() => csCursor.hasNext()); // Causes a getMore to be dispatched.
assert(csCursor.objsLeftInBatch() == 1);

// Because the retrieved event is the most recent entry in the oplog, the PBRT should be equal to
// the resume token of the last item in the batch and greater than the initial PBRT.
let resumeTokenFromDoc = csCursor.next()._id;
getMorePBRT = csCursor.getResumeToken();
// When running in a sharded passthrough, we cannot guarantee that the retrieved event was the last
// item in the oplog, and so we cannot assert that the PBRT is equal to the event's resume token.
assert.eqIfNotMongos(bsonWoCompare(getMorePBRT, resumeTokenFromDoc), 0);
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

// Test that postBatchResumeToken advances with getMore. Iterate the cursor and assert that the
// observed postBatchResumeToken advanced.
assert.soon(() => csCursor.hasNext()); // Causes a getMore to be dispatched.

// The postBatchResumeToken is again equal to the final token in the batch, and greater than the
// PBRT from the initial response.
let eventFromCursor = null;
while (csCursor.objsLeftInBatch()) {
    eventFromCursor = csCursor.next();
    resumeTokenFromDoc = eventFromCursor._id;
}
getMorePBRT = csCursor.getResumeToken();
// When running in a sharded passthrough, we cannot guarantee that the retrieved event was the last
// item in the oplog, and so we cannot assert that the PBRT is equal to the event's resume token.
assert.eqIfNotMongos(bsonWoCompare(resumeTokenFromDoc, getMorePBRT), 0);
assert.gt(bsonWoCompare(getMorePBRT, initialAggPBRT), 0);

// Test that postBatchResumeToken advances with writes to an unrelated collection. First make
// sure there is nothing left in our cursor, and obtain the latest PBRT...
while (eventFromCursor.fullDocument._id < docId - 1) {
    assert.soon(() => csCursor.hasNext());
    eventFromCursor = csCursor.next();
}
assert(!csCursor.hasNext());
let previousGetMorePBRT = csCursor.getResumeToken();
assert.neq(undefined, previousGetMorePBRT);

// ... then test that it advances on an insert to an unrelated collection.
assert.commandWorked(otherCollection.insert({_id: docId}));
assert.soon(() => {
    assert(!csCursor.hasNext()); // Causes a getMore to be dispatched.
    getMorePBRT = csCursor.getResumeToken();
    return bsonWoCompare(getMorePBRT, previousGetMorePBRT) > 0;
});

// Insert two documents into the collection which are of the maximum BSON object size.
const bsonUserSizeLimit = assert.commandWorked(adminDB.hello()).maxBsonObjectSize;
assert.gt(bsonUserSizeLimit, 0);
for (let i = 0; i < 2; ++i) {
    const docToInsert = {_id: docId++, padding: ""};
    docToInsert.padding = "a".repeat(bsonUserSizeLimit - Object.bsonsize(docToInsert));
    assert.commandWorked(testCollection.insert(docToInsert));
}

// Test that we return the correct postBatchResumeToken in the event that the batch hits the
// byte size limit. Despite the fact that the batchSize is 2, we should only see 1 result,
// because the second result cannot fit in the batch.
assert.soon(() => csCursor.hasNext()); // Causes a getMore to be dispatched.
assert.eq(csCursor.objsLeftInBatch(), 1);

// Obtain the resume token and the PBRT from the first document.
resumeTokenFromDoc = csCursor.next()._id;
getMorePBRT = csCursor.getResumeToken();

// Now retrieve the second event and confirm that the PBRTs and resume tokens are in-order.
previousGetMorePBRT = getMorePBRT;
assert.soon(() => csCursor.hasNext()); // Causes a getMore to be dispatched.
assert.eq(csCursor.objsLeftInBatch(), 1);
const resumeTokenFromSecondDoc = csCursor.next()._id;
getMorePBRT = csCursor.getResumeToken();
assert.gte(bsonWoCompare(previousGetMorePBRT, resumeTokenFromDoc), 0);
assert.gt(bsonWoCompare(resumeTokenFromSecondDoc, previousGetMorePBRT), 0);
assert.gte(bsonWoCompare(getMorePBRT, resumeTokenFromSecondDoc), 0);

// Sharded collection passthroughs use prepared transactions, which require majority read concern.
// If the collection is sharded and majority read concern is disabled, skip the transaction tests.
const rcCmdRes = testCollection.runCommand("find", {readConcern: {level: "majority"}});
if (FixtureHelpers.isSharded(testCollection) && rcCmdRes.code === ErrorCodes.ReadConcernMajorityNotEnabled) {
    jsTestLog("Skipping transaction tests since majority read concern is disabled.");
    quit();
}

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
assert.commandWorked(sessionOtherColl.insert({_id: docId}));
assert.commandWorked(session.commitTransaction_forTesting());
session.endSession();

// Grab the next 2 events, which should be the first 2 events in the transaction. As of SERVER-37364
// the co-ordinator of a distributed transaction returns before all participants have acknowledged
// the decision, and so not all events may yet be majority-visible. We therefore wait until we see
// both expected events in the first set of results retrieved from the transaction.
previousGetMorePBRT = getMorePBRT;
assert.soon(() => {
    // Start a new stream from the most recent resume token we retrieved.
    csCursor = testCollection.watch([], {resumeAfter: previousGetMorePBRT, cursor: {batchSize: batchSize}});
    // Wait until we see the first results from the stream.
    assert.soon(() => csCursor.hasNext());
    // There should be two distinct events in the batch.
    return csCursor.objsLeftInBatch() === 2;
});

// The clusterTime should be the same on each, but the resume token keeps advancing.
const txnEvent1 = csCursor.next(),
    txnEvent2 = csCursor.next();
const txnClusterTime = txnEvent1.clusterTime;
// On a sharded cluster, the events in the txn may be spread across multiple shards. Events from
// each shard will all have the same clusterTime, but the clusterTimes may differ between shards.
// Therefore, we cannot guarantee that the clusterTime of txnEvent2 is always the same as the
// clusterTime of txnEvent1, since the events may have occurred on different shards.
assert.eqIfNotMongos(txnEvent2.clusterTime, txnClusterTime);
assert.gt(bsonWoCompare(txnEvent1._id, previousGetMorePBRT), 0);
assert.gt(bsonWoCompare(txnEvent2._id, txnEvent1._id), 0);

// The PBRT of the first transaction batch is equal to the last document's resumeToken. We have
// more events to return from the transaction, and so the PBRT cannot have advanced any further.
getMorePBRT = csCursor.getResumeToken();
assert.eq(bsonWoCompare(getMorePBRT, txnEvent2._id), 0);

// Now get the next batch. This contains the third of the four transaction operations.
previousGetMorePBRT = getMorePBRT;
assert.soon(() => csCursor.hasNext()); // Causes a getMore to be dispatched.
assert.eq(csCursor.objsLeftInBatch(), 1);

// The clusterTime of this event is the same as the two events from the previous batch, but its
// resume token is greater than the previous PBRT.
const txnEvent3 = csCursor.next();
// As before, we cannot guarantee that the clusterTime of txnEvent3 is always the same as that of
// txnEvent1 when running in a sharded cluster. However, the PBRT should advance in any deployment.
assert.eqIfNotMongos(txnEvent3.clusterTime, txnClusterTime);
assert.gt(bsonWoCompare(txnEvent3._id, previousGetMorePBRT), 0);

// Because we wrote to the unrelated collection, the final event in the transaction does not
// appear in the batch. Confirm that the postBatchResumeToken has been set correctly.
getMorePBRT = csCursor.getResumeToken();
assert.gte(bsonWoCompare(getMorePBRT, txnEvent3._id), 0);

// Test that a batch does not exceed the limit (and throw BSONObjectTooLarge) with a large
// post-batch resume token.
csCursor = testCollection.watch();
const kSecondDocSize = 80 * 1024;
// Here, 4.5 is some "unlucky" (non-unique) weight to provoke an error.
const kFirstDocSize = 16 * 1024 * 1024 - 4.5 * kSecondDocSize;
testCollection.insertMany([{a: "x".repeat(kFirstDocSize)}, {_id: "x".repeat(kSecondDocSize)}]);
assert.doesNotThrow(
    () => {
        csCursor.hasNext();
    },
    [],
    "Unexpected exception on 'csCursor.hasNext()'.",
);
