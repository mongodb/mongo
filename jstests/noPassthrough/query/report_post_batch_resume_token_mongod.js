/**
 * Tests mongoD-specific semantics of postBatchResumeToken for $changeStream aggregations.
 * @tags: [
 *   requires_majority_read_concern,
 *   uses_transactions,
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Create a new single-node replica set, and ensure that it can support $changeStream.
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB(jsTestName());
const collName = "report_post_batch_resume_token";
const testCollection = assertDropAndRecreateCollection(db, collName);
const otherCollection = assertDropAndRecreateCollection(db, "unrelated_" + collName);
const oplogColl = db.getSiblingDB("local").oplog.rs;

let docId = 0; // Tracks _id of documents inserted to ensure that we do not duplicate.
const batchSize = 2;

// Helper function to perform generic comparisons and dump the oplog on failure.
function assertCompare(cmpFn, left, right, cmpOp, cmpVal) {
    assert[cmpOp](cmpFn(left, right), cmpVal, {left: left, right: right, oplogEntries: oplogColl.find().toArray()});
}

// Start watching the test collection in order to capture a resume token.
let csCursor = testCollection.watch();

// Write some documents to the test collection and get the resume token from the first doc.
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(testCollection.insert({_id: docId++}));
}

assert.soon(() => csCursor.hasNext());
const resumeTokenFromDoc = csCursor.next()._id;
csCursor.close();

// Test that postBatchResumeToken is present on a non-empty initial aggregate batch.
assert.soon(() => {
    csCursor = testCollection.watch([], {resumeAfter: resumeTokenFromDoc});
    assert.soon(() => csCursor.hasNext());
    csCursor.close(); // We don't need any results after the initial batch.
    return csCursor.objsLeftInBatch();
});
while (csCursor.objsLeftInBatch()) {
    csCursor.next();
}
let initialAggPBRT = csCursor.getResumeToken();
assert.neq(undefined, initialAggPBRT);

// Test that the PBRT is correctly updated when reading events from within a transaction.
const session = db.getMongo().startSession();
const sessionDB = session.getDatabase(db.getName());

const sessionColl = sessionDB[testCollection.getName()];
const sessionOtherColl = sessionDB[otherCollection.getName()];
session.startTransaction();

// Open a stream of batchSize:2 and grab the PBRT of the initial batch.
csCursor = testCollection.watch([], {cursor: {batchSize: batchSize}});
initialAggPBRT = csCursor.getResumeToken();
assert.eq(csCursor.objsLeftInBatch(), 0);

// Write 3 documents to testCollection and 1 to the unrelated collection within the transaction.
for (let i = 0; i < 3; ++i) {
    assert.commandWorked(sessionColl.insert({_id: docId++}));
}
assert.commandWorked(sessionOtherColl.insert({}));
assert.commandWorked(session.commitTransaction_forTesting());
session.endSession();

// Grab the next 2 events, which should be the first 2 events in the transaction.
assert(csCursor.hasNext()); // Causes a getMore to be dispatched.
assert.eq(csCursor.objsLeftInBatch(), 2);

// The clusterTime should be the same on each, but the resume token keeps advancing.
const txnEvent1 = csCursor.next(),
    txnEvent2 = csCursor.next();
const txnClusterTime = txnEvent1.clusterTime;
assertCompare(timestampCmp, txnEvent2.clusterTime, txnClusterTime, "eq", 0);
assertCompare(bsonWoCompare, txnEvent1._id, initialAggPBRT, "gt", 0);
assertCompare(bsonWoCompare, txnEvent2._id, txnEvent1._id, "gt", 0);

// The PBRT of the first transaction batch is equal to the last document's resumeToken.
let getMorePBRT = csCursor.getResumeToken();
assertCompare(bsonWoCompare, getMorePBRT, txnEvent2._id, "eq", 0);

// Save this PBRT so that we can test resuming from it later on.
const resumePBRT = getMorePBRT;

// Now get the next batch. This contains the third of the four transaction operations.
let previousGetMorePBRT = getMorePBRT;
assert(csCursor.hasNext()); // Causes a getMore to be dispatched.
assert.eq(csCursor.objsLeftInBatch(), 1);

// The clusterTime of this event is the same as the two events from the previous batch, but its
// resume token is greater than the previous PBRT.
const txnEvent3 = csCursor.next();
assertCompare(timestampCmp, txnEvent3.clusterTime, txnClusterTime, "eq", 0);
assertCompare(bsonWoCompare, txnEvent3._id, previousGetMorePBRT, "gt", 0);

// Because we wrote to the unrelated collection, the final event in the transaction does not
// appear in the batch. But in this case it also does not allow our PBRT to advance beyond the
// last event in the batch, because the unrelated event is within the same transaction and
// therefore has the same clusterTime.
getMorePBRT = csCursor.getResumeToken();
assertCompare(bsonWoCompare, getMorePBRT, txnEvent3._id, "eq", 0);

// Confirm that resuming from the PBRT of the first batch gives us the third transaction write.
csCursor = testCollection.watch([], {resumeAfter: resumePBRT});
assert.soon(() => csCursor.hasNext());
assert.docEq(csCursor.next(), txnEvent3);
assert(!csCursor.hasNext());

rst.stopSet();
