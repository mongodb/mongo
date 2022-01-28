//
// Tests that refineCollectionShardKey deletes all existing chunks in the persisted routing table
// cache.
//

(function() {
'use strict';

const st = new ShardingTest({shards: 1});
const mongos = st.s0;
const shard = st.shard0;
const kDbName = 'db';
const kCollName = 'foo';
const kNsName = kDbName + '.' + kCollName;
const oldKeyDoc = {
    a: 1,
    b: 1
};
const newKeyDoc = {
    a: 1,
    b: 1,
    c: 1,
    d: 1
};

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: oldKeyDoc}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

// Ensure that there exist three chunks belonging to 'db.foo' covering the entire key range.
//
// Chunk 1: {a: MinKey, b: MinKey} -->> {a: 0, b: 0}
// Chunk 2: {a: 0, b: 0} -->> {a: 5, b: 5}
// Chunk 3: {a: 5, b: 5} -->> {a: MaxKey, b: MaxKey}
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 0, b: 0}}));
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 5, b: 5}}));

// Flush the routing table cache and verify that 'config.cache.chunks.db.foo' is as expected
// before refineCollectionShardKey.
assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: kNsName}));

let collEntry = st.config.collections.findOne({_id: kNsName});
let configCacheChunks = "config.cache.chunks." + kNsName;
let chunkArr = shard.getCollection(configCacheChunks).find({}).sort({min: 1}).toArray();
assert.eq(3, chunkArr.length);
assert.eq({a: MinKey, b: MinKey}, chunkArr[0]._id);
assert.eq({a: 0, b: 0}, chunkArr[0].max);
assert.eq({a: 0, b: 0}, chunkArr[1]._id);
assert.eq({a: 5, b: 5}, chunkArr[1].max);
assert.eq({a: 5, b: 5}, chunkArr[2]._id);
assert.eq({a: MaxKey, b: MaxKey}, chunkArr[2].max);

assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));

// Verify that 'config.cache.chunks.db.foo' is as expected after refineCollectionShardKey. NOTE: We
// use assert.soon here because refineCollectionShardKey doesn't block for each shard to refresh.
assert.soon(() => {
    let collectionCacheArr =
        shard.getCollection('config.cache.collections').find({_id: kNsName}).toArray();

    chunkArr = shard.getCollection(configCacheChunks).find({}).sort({min: 1}).toArray();
    return collectionCacheArr.length === 1 && collectionCacheArr[0].epoch != collEntry.epoch &&
        3 === chunkArr.length;
});
assert.eq({a: MinKey, b: MinKey, c: MinKey, d: MinKey}, chunkArr[0]._id);
assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, chunkArr[0].max);
assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, chunkArr[1]._id);
assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, chunkArr[1].max);
assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, chunkArr[2]._id);
assert.eq({a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}, chunkArr[2].max);

st.stop();
})();
