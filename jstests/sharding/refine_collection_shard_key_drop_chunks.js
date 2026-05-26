//
// Tests that refineCollectionShardKey deletes all existing chunks in the persisted routing table
// cache.
//

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const st = new ShardingTest({shards: 1});
const mongos = st.s0;
const shard = st.shard0;
const kDbName = "db";
const kCollName = "foo";
const kNsName = kDbName + "." + kCollName;
const kCachedCollectionsNs = "config.cache.collections";
const kCacheChunksNs = "config.cache.chunks." + kNsName;
const kAuthoritativeCollectionsNs = "config.shard.catalog.collections";
const kAuthoritativeChunksNs = "config.shard.catalog.chunks";
const oldKeyDoc = {
    a: 1,
    b: 1,
};
const newKeyDoc = {
    a: 1,
    b: 1,
    c: 1,
    d: 1,
};

const isAuthoritativeMetadata = FeatureFlagUtil.isEnabled(st.s.getDB("admin"), "featureFlagAuthoritativeShardsDDL");

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
// TODO (SERVER-125785): with isAuthoritativeMetadata = true and authoritative split, this
// _flushRoutingTableCacheUpdates can be removed and check
// kAuthoritativeCollectionsNs/kCacheChunksNs instead of kCachedCollectionsNs/kCacheChunksNs.
assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: kNsName}));

let collEntry = shard.getCollection(kCachedCollectionsNs).findOne({_id: kNsName});
let chunkArr = shard.getCollection(kCacheChunksNs).find({}).sort({min: 1}).toArray();
assert.eq(3, chunkArr.length);
assert.eq({a: MinKey, b: MinKey}, chunkArr[0]._id);
assert.eq({a: 0, b: 0}, chunkArr[0].max);
assert.eq({a: 0, b: 0}, chunkArr[1]._id);
assert.eq({a: 5, b: 5}, chunkArr[1].max);
assert.eq({a: 5, b: 5}, chunkArr[2]._id);
assert.eq({a: MaxKey, b: MaxKey}, chunkArr[2].max);

assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));

// refineCollectionShardKey doesn't block for each shard to refresh, so wait until the cached
// information is fully up to date.
assert.soon(() => {
    const kCollectionsNs = isAuthoritativeMetadata ? kAuthoritativeCollectionsNs : kCachedCollectionsNs;
    let newCollEntry = shard.getCollection(kCollectionsNs).findOne({_id: kNsName});
    return newCollEntry.epoch != collEntry.epoch && !newCollEntry.refreshing;
});

// Verify that the chunks collection is as expected after refineCollectionShardKey.
let minFieldName;
if (isAuthoritativeMetadata) {
    const uuid = shard.getCollection(kAuthoritativeCollectionsNs).findOne({_id: kNsName}).uuid;
    chunkArr = shard.getCollection(kAuthoritativeChunksNs).find({uuid: uuid}).sort({min: 1}).toArray();
    minFieldName = "min";
} else {
    chunkArr = shard.getCollection(kCacheChunksNs).find({}).sort({min: 1}).toArray();
    minFieldName = "_id";
}

assert.eq(3, chunkArr.length);
assert.eq({a: MinKey, b: MinKey, c: MinKey, d: MinKey}, chunkArr[0][minFieldName]);
assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, chunkArr[0].max);
assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, chunkArr[1][minFieldName]);
assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, chunkArr[1].max);
assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, chunkArr[2][minFieldName]);
assert.eq({a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}, chunkArr[2].max);

st.stop();
