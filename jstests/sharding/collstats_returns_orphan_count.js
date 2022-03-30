/**
 * Test that collstats returns the correct number of orphaned documents.
 *
 * @tags: [
 *  requires_fcv_60,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const rangeDeleterBatchSize = 128;

const st = new ShardingTest({
    shards: 2,
    other: {
        shardOptions: {setParameter: {rangeDeleterBatchSize: rangeDeleterBatchSize}},
    }
});

function assertCollStatsHasCorrectOrphanCount(coll, shardName, numOrphans) {
    const pipeline = [
        {'$collStats': {'storageStats': {}}},
        {'$project': {'shard': true, 'storageStats': {'numOrphanDocs': true}}}
    ];
    const storageStats = coll.aggregate(pipeline).toArray();
    storageStats.forEach((stat) => {
        if (stat['shard'] === shardName) {
            assert.eq(stat.storageStats.numOrphanDocs, numOrphans);
        }
    });
}

// Setup database
const dbName = 'db';
const db = st.getDB(dbName);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Test non-existing collection
const noColl = db['unusedColl'];
let res = db.runCommand({'collStats': noColl.getFullName()});
assert.eq(res.shards[st.shard0.shardName].numOrphanDocs, 0);

// Setup collection for test with orphans
const coll = db['test'];
const nss = coll.getFullName();
assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));

// Create two chunks
assert.commandWorked(st.s.adminCommand({split: nss, middle: {_id: 0}}));

// Insert 1000 docs into the chunk we will move.
const numDocs = 1000;
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({_id: i});
}

// Insert 10 docs into the chunk we will not move.
const numDocsUnmoved = 10;
for (let i = -numDocsUnmoved; i < 0; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

// Check there are no orphans before the chunk is moved
assertCollStatsHasCorrectOrphanCount(coll, st.shard1.shardName, 0);

// Pause before first range deletion task
let beforeDeletionFailpoint = configureFailPoint(st.shard0, "hangBeforeDoingDeletion");
let afterDeletionFailpoint = configureFailPoint(st.shard0, "hangAfterDoingDeletion");
assert.commandWorked(db.adminCommand({moveChunk: nss, find: {_id: 0}, to: st.shard1.shardName}));

// Check the batches are deleted correctly
const numBatches = numDocs / rangeDeleterBatchSize;
for (let i = 0; i < numBatches; i++) {
    // Wait for failpoint and check num orphans
    beforeDeletionFailpoint.wait();
    assertCollStatsHasCorrectOrphanCount(
        coll, st.shard0.shardName, numDocs - rangeDeleterBatchSize * i);
    // Unset and reset failpoint without allowing any batches deleted in the meantime
    afterDeletionFailpoint = configureFailPoint(st.shard0, "hangAfterDoingDeletion");
    beforeDeletionFailpoint.off();
    afterDeletionFailpoint.wait();
    beforeDeletionFailpoint = configureFailPoint(st.shard0, "hangBeforeDoingDeletion");
    afterDeletionFailpoint.off();
}
beforeDeletionFailpoint.off();

st.stop();
})();
