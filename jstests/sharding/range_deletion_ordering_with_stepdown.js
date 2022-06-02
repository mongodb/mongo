/**
 * Test that an ongoing range deletion is the first range deletion executed upon step up.
 *
 * @tags: [
 *  requires_fcv_60,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const rangeDeleterBatchSize = 128;

const st = new ShardingTest(
    {shards: 2, rs: {nodes: 2, setParameter: {rangeDeleterBatchSize: rangeDeleterBatchSize}}});

// Setup database
const dbName = 'db';
const db = st.getDB(dbName);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Setup collection for test with orphans
const coll = db['test'];
const nss = coll.getFullName();
assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));

// Create two chunks
assert.commandWorked(st.s.adminCommand({split: nss, middle: {_id: 0}}));

// Insert 1000 docs into both chunks.
const numDocs = 1000;
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= numDocs; i++) {
    bulk.insert({_id: i});
    bulk.insert({_id: -i});
}
assert.commandWorked(bulk.execute());

// Pause before first range deletion task
let beforeDeletionStarts = configureFailPoint(st.shard0, "suspendRangeDeletion");
assert.commandWorked(db.adminCommand({moveChunk: nss, find: {_id: 1}, to: st.shard1.shardName}));
assert.commandWorked(db.adminCommand({moveChunk: nss, find: {_id: -1}, to: st.shard1.shardName}));

// Allow first batch from one of the ranges to be deleted
let beforeDeletionFailpoint = configureFailPoint(st.shard0, "hangBeforeDoingDeletion");
beforeDeletionStarts.off();
beforeDeletionFailpoint.wait();
let afterDeletionFailpoint = configureFailPoint(st.shard0, "hangAfterDoingDeletion");
beforeDeletionFailpoint.off();
afterDeletionFailpoint.wait();

// Figure out which range had a batch deleted from it
let rangeDeletionDocs = st.shard0.getDB("config").getCollection("rangeDeletions").find().toArray();
assert.eq(rangeDeletionDocs.length, 2);
let processingDoc, otherDoc;
if (rangeDeletionDocs[0].numOrphanDocs.valueOf() === numDocs) {
    assert.eq(rangeDeletionDocs[1].numOrphanDocs, numDocs - rangeDeleterBatchSize);
    processingDoc = rangeDeletionDocs[1];
    otherDoc = rangeDeletionDocs[0];
} else {
    assert.eq(rangeDeletionDocs[0].numOrphanDocs, numDocs - rangeDeleterBatchSize);
    assert.eq(rangeDeletionDocs[1].numOrphanDocs, numDocs);
    processingDoc = rangeDeletionDocs[0];
    otherDoc = rangeDeletionDocs[1];
}

// Reorder the tasks on disk to make it more likely they would be submitted out of order
assert.commandWorked(st.shard0.getDB("config").getCollection("rangeDeletions").deleteMany({}));
assert.commandWorked(
    st.shard0.getDB("config").getCollection("rangeDeletions").insert(rangeDeletionDocs[1]));
assert.commandWorked(
    st.shard0.getDB("config").getCollection("rangeDeletions").insert(rangeDeletionDocs[0]));

// Step down
let newPrimary = st.rs0.getSecondaries()[0];
beforeDeletionFailpoint = configureFailPoint(newPrimary, "hangBeforeDoingDeletion");
st.rs0.stepUp(newPrimary);

// Allow another batch deletion
afterDeletionFailpoint.off();
beforeDeletionFailpoint.wait();
afterDeletionFailpoint = configureFailPoint(st.shard0, "hangAfterDoingDeletion");
beforeDeletionFailpoint.off();
afterDeletionFailpoint.wait();

// Make sure the batch deleted was from the same range deletion
rangeDeletionDocs = st.shard0.getDB("config").getCollection("rangeDeletions").find().toArray();
assert.eq(rangeDeletionDocs.length, 2);
rangeDeletionDocs.forEach((doc) => {
    if (bsonWoCompare(processingDoc.range, doc.range) === 0) {
        jsTest.log("Same id: " + tojson(doc));
        assert.eq(doc.numOrphanDocs, numDocs - 2 * rangeDeleterBatchSize);
    } else {
        jsTest.log("Diff id: " + tojson(doc));
        assert.eq(doc.numOrphanDocs, numDocs);
    }
});

// Allow everything to finish
afterDeletionFailpoint.off();

st.stop();
})();
