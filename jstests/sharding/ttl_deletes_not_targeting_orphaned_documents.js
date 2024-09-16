/**
 * Test that deletes triggered by TTL index do not affect orphaned documents
 *
 * @tags: [requires_fcv_60]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

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

// Shard a collection on _id:1 so that the initial chunk will reside on the primary shard (shard0)
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: collName, key: {_id: 1}}));

// Insert documents that are going to be deleted by the TTL index created later on
const currTime = new Date();
var bulk = coll.initializeUnorderedBulkOp();
const nDocs = 100;
for (let i = 0; i < nDocs; i++) {
    bulk.insert({_id: i, a: currTime});
}
assert.commandWorked(bulk.execute());

// Move all documents to the other shard (shard1) but keep a chunk on shard0 to create the TTL index
assert.commandWorked(st.s.adminCommand({split: collName, middle: {_id: -1}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: collName, find: {_id: 0}, to: st.shard1.shardName}));

// Initialize TTL index: delete documents with field `a: <current date>` older than 1 second
assert.commandWorked(coll.createIndex({a: 1}, {expireAfterSeconds: 1}));

// Verify that TTL index worked properly on owned documents on shard1
assert.soon(function() {
    return coll.countDocuments({}) == 0;
}, "Failed to move all documents", 60000 /* 60 seconds */, 5000 /* 5 seconds */);

// Verify that TTL index did not delete orphaned documents on shard0
assert.eq(nDocs, st.rs0.getPrimary().getCollection(collName).countDocuments({}));

st.stop();
