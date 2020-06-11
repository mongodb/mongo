/**
 * Refine shard key currently only changes the epoch and does not change the major/minor versions
 * in the chunks. This test simulates a stepDown in the middle of making changes to the
 * config.cache collections by the ShardServerCatalogCacheLoader after a refine shard key and makes
 * sure that the shard will be able to eventually reach the valid state on config.cache.
 *
 * @tags: [requires_fcv_46]
 */
(function() {
'use strict';

let st = new ShardingTest({shards: 1});

let testDB = st.s.getDB('test');

assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {x: 1}}));

assert.commandWorked(st.s.adminCommand({split: 'test.user', middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({split: 'test.user', middle: {x: 10}}));

assert.commandWorked(testDB.user.insert({x: 1, y: 1}));
assert.commandWorked(testDB.user.insert({x: 10, y: 1}));
testDB.user.ensureIndex({x: 1, y: 1});

let priConn = st.rs0.getPrimary();
assert.commandWorked(
    priConn.adminCommand({_flushRoutingTableCacheUpdates: 'test.user', syncFromConfig: true}));

let chunkCache = priConn.getDB('config').cache.chunks.test.user;
let preRefineChunks = chunkCache.find().toArray();
assert.eq(3, preRefineChunks.length);

assert.commandWorked(st.s.adminCommand({refineCollectionShardKey: 'test.user', key: {x: 1, y: 1}}));

// Force refresh the config.cache.collections and config.cache.chunks, then manually revert
// the config.cache.chunks to simulate crash before updating config.cache.chunks.
assert.commandWorked(priConn.adminCommand({_flushRoutingTableCacheUpdates: 'test.user'}));

assert.commandWorked(chunkCache.remove({}, false /* justOne */));
preRefineChunks.forEach((chunk) => {
    assert.commandWorked(chunkCache.insert(chunk));
});

let collCache = priConn.getDB('config').cache.collections;
assert.commandWorked(collCache.update({_id: 'test.user'}, {$set: {refreshing: true}}));

// Force refresh should be able to detect anomaly and fix the cache collections.
assert.commandWorked(priConn.adminCommand({_flushRoutingTableCacheUpdates: 'test.user'}));

let fixedChunks = chunkCache.find().sort({min: 1}).toArray();
assert.eq(3, fixedChunks.length);

assert.eq({x: MinKey, y: MinKey}, fixedChunks[0]._id);
assert.eq({x: 0, y: MinKey}, fixedChunks[1]._id);
assert.eq({x: 10, y: MinKey}, fixedChunks[2]._id);

let collDoc = collCache.findOne({_id: 'test.user'});
assert.eq(false, collDoc.refreshing);

st.stop();
}());
