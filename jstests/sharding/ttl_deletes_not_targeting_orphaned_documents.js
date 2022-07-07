/**
 * Test that deletes triggered by TTL index do not affect orphaned documents
 *
 * @tags: [requires_fcv_60]
 */
(function() {
"use strict";
// The range deleter is disabled for this test, hence orphans are not cleared up
TestData.skipCheckOrphans = true;

const st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: 1,
        // Reduce TTL Monitor sleep and disable the range deleter
        setParameter: {ttlMonitorSleepSecs: 1, disableResumableRangeDeleter: true}
    }
});
const dbName = 'test';
const testDB = st.s.getDB('test');
const coll = testDB[jsTest.name()];
const collName = coll.getFullName();

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: collName, key: {_id: 1}}));

// Initialize TTL index: delete documents with field `a: <current date>` after 10 seconds
assert.commandWorked(coll.createIndex({a: 1}, {expireAfterSeconds: 10}));

// Insert documents that are going to be deleted in 10 seconds
const currTime = new Date();
var bulk = coll.initializeUnorderedBulkOp();
const nDocs = 100;
for (let i = 0; i < nDocs; i++) {
    bulk.insert({_id: i, a: currTime});
}
assert.commandWorked(bulk.execute());

// Move all documents on other shards
assert.commandWorked(
    st.s.adminCommand({moveChunk: collName, find: {_id: 0}, to: st.shard1.shardName}));

// Verify that TTL index worked properly on owned documents
assert.soon(function() {
    return coll.countDocuments({}) == 0;
}, "Failed to move all documents", 60000 /* 60 seconds */, 5000 /* 5 seconds */);

// Verify that TTL index did not delete orphaned documents
assert.eq(nDocs, st.rs0.getPrimary().getCollection(collName).countDocuments({}));

st.stop();
})();
