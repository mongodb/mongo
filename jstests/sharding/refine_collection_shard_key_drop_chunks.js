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
const kShardCatalogCollectionsNs = isAuthoritativeMetadata ? kAuthoritativeCollectionsNs : kCachedCollectionsNs;

// Returns the persisted collection metadata entry for 'kNsName' from the shard catalog, reading
// from the authoritative or the cached collection depending on the feature flag.
function getCollEntry() {
    return shard.getCollection(kShardCatalogCollectionsNs).findOne({_id: kNsName});
}

// Asserts that the chunks persisted on the shard match 'expectedChunks', an array of
// {min, max} boundaries sorted ascending. Reads from the authoritative shard catalog
// (config.shard.catalog.chunks, filtered by collection uuid, with boundaries in 'min'/'max') or
// from the routing table cache (config.cache.chunks.<ns>, where '_id' holds the min) depending on
// the feature flag.
function assertChunks(expectedChunks) {
    let chunkArr;
    let minFieldName;
    if (isAuthoritativeMetadata) {
        const uuid = getCollEntry().uuid;
        chunkArr = shard.getCollection(kAuthoritativeChunksNs).find({uuid: uuid}).sort({min: 1}).toArray();
        minFieldName = "min";
    } else {
        chunkArr = shard.getCollection(kCacheChunksNs).find({}).sort({_id: 1}).toArray();
        minFieldName = "_id";
    }

    assert.eq(expectedChunks.length, chunkArr.length, {chunkArr});
    expectedChunks.forEach((expected, i) => {
        assert.eq(expected.min, chunkArr[i][minFieldName], {chunkArr});
        assert.eq(expected.max, chunkArr[i].max, {chunkArr});
    });
}

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

if (!isAuthoritativeMetadata) {
    // Flush the routing table cache and verify that 'config.cache.chunks.db.foo' is as expected
    // before refineCollectionShardKey.
    assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: kNsName}));
}

let collEntry = getCollEntry();
assertChunks([
    {min: {a: MinKey, b: MinKey}, max: {a: 0, b: 0}},
    {min: {a: 0, b: 0}, max: {a: 5, b: 5}},
    {min: {a: 5, b: 5}, max: {a: MaxKey, b: MaxKey}},
]);

assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));

if (isAuthoritativeMetadata) {
    let newCollEntry = getCollEntry();
    assert.neq(newCollEntry.lastmodEpoch, collEntry.lastmodEpoch);
    assert.neq(newCollEntry.timestamp, collEntry.timestamp);
} else {
    // refineCollectionShardKey doesn't block for each shard to refresh, so wait until the cached
    // information is fully up to date.
    assert.soon(() => {
        let newCollEntry = getCollEntry();
        return newCollEntry.epoch != collEntry.epoch && !newCollEntry.refreshing;
    });
}

// Verify that the chunks collection is as expected after refineCollectionShardKey.
assertChunks([
    {min: {a: MinKey, b: MinKey, c: MinKey, d: MinKey}, max: {a: 0, b: 0, c: MinKey, d: MinKey}},
    {min: {a: 0, b: 0, c: MinKey, d: MinKey}, max: {a: 5, b: 5, c: MinKey, d: MinKey}},
    {min: {a: 5, b: 5, c: MinKey, d: MinKey}, max: {a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}},
]);

st.stop();
