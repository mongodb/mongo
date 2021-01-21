/*
 * Test that $merge sends results to the right shards.
 */
(function() {
'use strict';

load("jstests/aggregation/extras/merge_helpers.js");
load("jstests/sharding/libs/chunk_bounds_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

let st = new ShardingTest({shards: 3});
let dbName = "test";
let configDB = st.s.getDB('config');
let testDB = st.s.getDB(dbName);
let sourceColl = testDB.source;
let targetColl = testDB.target;
let sourceNs = sourceColl.getFullName();
let targetNs = targetColl.getFullName();

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: targetNs, key: {x: 'hashed'}}));

let chunkDocsForTargetColl = findChunksUtil.findChunksByNs(configDB, targetNs).toArray();
let shardChunkBoundsForTargetColl = chunkBoundsUtil.findShardChunkBounds(chunkDocsForTargetColl);

// Use docs that are expected to go to three different shards.
let docs = [{x: -10}, {x: -1}, {x: 10}];
assert.commandWorked(sourceColl.insert(docs));
let shards = docs.map((doc) => {
    let hash = convertShardKeyToHashed(doc.x);
    return chunkBoundsUtil.findShardForShardKey(st, shardChunkBoundsForTargetColl, {x: hash});
});
assert.eq(3, (new Set(shards)).size);

// Run aggregation with $merge. Use $set to differentiate between the original and
// merged docs.
assert.commandWorked(sourceColl.runCommand({
    aggregate: sourceColl.getName(),
    pipeline: [{$match: {}}, {$set: {y: 1}}, {$merge: {into: targetColl.getName()}}],
    cursor: {}
}));

// Check that the merged docs end up on the right shards and that the original docs are still
// on the primary shard.
let primaryShard = st.getPrimaryShard(dbName);
for (let i = 0; i < docs.length; i++) {
    let mergedDoc = Object.assign({y: 1}, docs[i]);
    assert.eq(1, primaryShard.getCollection(sourceNs).count(docs[i]));
    assert.eq(1, shards[i].getCollection(targetNs).count(mergedDoc));
}

st.stop();
})();
