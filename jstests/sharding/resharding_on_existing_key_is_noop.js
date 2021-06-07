/**
 * Tests that trying to perform reshardCollection with a resharding key that matches the
 * collection's existing shard key is a noop (which can be done by confirming the collection's UUID
 * remains unchanged after the operation).
 *
 * @tags: [uses_atclustertime, requires_fcv_49,]
 */
(function() {
"use strict";

load("jstests/libs/uuid_util.js");
load("jstests/sharding/libs/create_sharded_collection_util.js");

const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 2},
});

const sourceCollection = st.s.getCollection("reshardingDb.coll");

CreateShardedCollectionUtil.shardCollectionWithChunks(sourceCollection, {key: 1}, [
    {min: {key: MinKey}, max: {key: 0}, shard: st.shard0.shardName},
    {min: {key: 0}, max: {key: MaxKey}, shard: st.shard1.shardName},
]);

const ns = sourceCollection.getFullName();
const mongos = sourceCollection.getMongo();
const sourceDB = sourceCollection.getDB();

// The UUID should remain the same if the resharding key matches the existing shard key.
const preReshardCollectionUUID = getUUIDFromListCollections(sourceDB, sourceCollection.getName());
assert.commandWorked(mongos.adminCommand({reshardCollection: ns, key: {key: 1}}));
const postReshardCollectionUUID = getUUIDFromListCollections(sourceDB, sourceCollection.getName());
assert.eq(preReshardCollectionUUID, postReshardCollectionUUID);

st.stop();
})();
