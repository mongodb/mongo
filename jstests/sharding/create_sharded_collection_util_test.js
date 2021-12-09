/**
 * Tests for the create_sharded_collection_util.js module.
 */
(function() {
"use strict";

load("jstests/sharding/libs/create_sharded_collection_util.js");

const kDbName = "test";
const kCollName = "create_sharded_collection_util";
const st = new ShardingTest({mongos: 1, config: 1, shards: 3, rs: {nodes: 1}});
const collection = st.s.getCollection(`${kDbName}.${kCollName}`);

function assertCreatedWithChunks(shardKey, chunks) {
    collection.drop();
    CreateShardedCollectionUtil.shardCollectionWithChunks(collection, shardKey, chunks);

    const configDB = st.s.getDB("config");
    const actualChunks =
        configDB.chunks.find({ns: collection.getFullName()}).sort({min: 1}).toArray();
    assert.eq(chunks.slice().sort((a, b) => bsonWoCompare(a.min, b.min)),
              actualChunks.map(chunk => ({min: chunk.min, max: chunk.max, shard: chunk.shard})));

    // CreateShardedCollectionUtil.shardCollectionWithChunks() should have cleaned up any zones it
    // generated temporarily.
    assert.eq([], configDB.tags.find().toArray());
    assert.eq([], configDB.shards.find({tags: {$ne: []}}).toArray());
}

// Can create a chunk on each one of shards.
assertCreatedWithChunks({a: 1}, [
    {min: {a: MinKey}, max: {a: 0}, shard: st.shard0.shardName},
    {min: {a: 0}, max: {a: 10}, shard: st.shard1.shardName},
    {min: {a: 10}, max: {a: MaxKey}, shard: st.shard2.shardName},
]);

// Can omit creating chunks on one of the shards.
assertCreatedWithChunks({a: 1}, [
    {min: {a: MinKey}, max: {a: 0}, shard: st.shard0.shardName},
    {min: {a: 0}, max: {a: 10}, shard: st.shard0.shardName},
    {min: {a: 10}, max: {a: 20}, shard: st.shard1.shardName},
    {min: {a: 20}, max: {a: 30}, shard: st.shard1.shardName},
    {min: {a: 30}, max: {a: MaxKey}, shard: st.shard1.shardName},
]);

// Can have the input list of chunk ranges not be sorted.
assertCreatedWithChunks({a: 1}, [
    {min: {a: 0}, max: {a: 10}, shard: st.shard0.shardName},
    {min: {a: 30}, max: {a: MaxKey}, shard: st.shard0.shardName},
    {min: {a: 10}, max: {a: 20}, shard: st.shard0.shardName},
    {min: {a: MinKey}, max: {a: 0}, shard: st.shard0.shardName},
    {min: {a: 20}, max: {a: 30}, shard: st.shard0.shardName},
]);

// Can shard using compound shard key.
assertCreatedWithChunks({a: 1, b: 1}, [
    {min: {a: MinKey, b: MinKey}, max: {a: 0, b: 0}, shard: st.shard0.shardName},
    {min: {a: 0, b: 0}, max: {a: 0, b: 10}, shard: st.shard1.shardName},
    {min: {a: 0, b: 10}, max: {a: MaxKey, b: MaxKey}, shard: st.shard2.shardName},
]);

// Cannot shard using hashed shard key.
// Due to a bug in the _shardsvrShardCollection command in v4.2, the hashed shard key doesn't work.
// It will fail due to the MinKey and MaxKey types not being supported. Instead it only works with
// NumberLong.
assertFailToCreateWithChunks(
    {a: "hashed"},
    [
        {min: {a: MinKey}, max: {a: NumberLong(0)}, shard: st.shard0.shardName},
        {min: {a: NumberLong(0)}, max: {a: NumberLong(10)}, shard: st.shard1.shardName},
        {min: {a: NumberLong(10)}, max: {a: MaxKey}, shard: st.shard2.shardName},
    ],
    /boundaries are not of type NumberLong/);

function assertFailToCreateWithChunks(shardKey, chunks, errRegex) {
    collection.drop();
    const err = assert.throws(
        () => CreateShardedCollectionUtil.shardCollectionWithChunks(collection, shardKey, chunks));

    assert(errRegex.test(err.message),
           `${tojson(errRegex)} didn't match error message: ${tojson(err)}`);
}

// Cannot have chunks array be empty.
assertFailToCreateWithChunks({a: 1}, [], /empty/);

// Cannot omit MinKey chunk.
assertFailToCreateWithChunks({a: 1},
                             [
                                 {min: {a: 0}, max: {a: 10}, shard: st.shard1.shardName},
                                 {min: {a: 10}, max: {a: MaxKey}, shard: st.shard2.shardName},
                             ],
                             /MinKey/);

// Cannot omit MaxKey chunk.
assertFailToCreateWithChunks({a: 1},
                             [
                                 {min: {a: MinKey}, max: {a: 0}, shard: st.shard0.shardName},
                                 {min: {a: 0}, max: {a: 10}, shard: st.shard1.shardName},
                             ],
                             /MaxKey/);

// Cannot have any gaps between chunks.
assertFailToCreateWithChunks({a: 1},
                             [
                                 {min: {a: MinKey}, max: {a: 0}, shard: st.shard0.shardName},
                                 {min: {a: 10}, max: {a: MaxKey}, shard: st.shard2.shardName},
                             ],
                             /found gap/);

st.stop();
})();
