/**
 * Ensure that the query plan cache will not block the removal of orphaned documents.
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getPlanCacheKeyFromShape.

const dbName = "test";
const collName = "sbe_plan_cache_does_not_block_range_deletion";
const ns = dbName + "." + collName;

const st = new ShardingTest({mongos: 1, config: 1, shards: 2});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

const coll = st.s.getDB(dbName)[collName];

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

assert.commandWorked(coll.insert({_id: 0, a: "abc", b: "123"}));

// Run the same query twice to create an active plan cache entry whose plan uses index {a: 1}.
for (let i = 0; i < 2; ++i) {
    assert.eq(coll.find({a: "abc", b: "123"}).itcount(), 1);
}

// Ensure there is a cache entry we just created in the plan cache.
const keyHash = getPlanCacheKeyFromShape(
    {query: {a: "abc", b: "123"}, collection: coll, db: st.s.getDB(dbName)});
const res = coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}]).toArray();
assert.eq(1, res.length);

// Move the chunk to the second shard leaving orphaned documents on the first shard.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.name}));

assert.soon(() => {
    // Ensure that the orphaned documents can be deleted.
    //
    // The "rangeDeletions" collection exists on each shard and stores a document for each chunk
    // range that contains orphaned documents. When the orphaned chunk range is cleaned up, the
    // document describing the range is deleted from the collection.
    return st.shard0.getDB('config')["rangeDeletions"].find().itcount() === 0;
});

st.stop();
})();
