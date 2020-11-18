/**
 * Tests the cloning portion of a resharding operation as part of the reshardCollection command.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/uuid_util.js");
load("jstests/sharding/libs/create_sharded_collection_util.js");

const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 1},
    rsOptions: {
        setParameter: {
            "failpoint.WTPreserveSnapshotHistoryIndefinitely": tojson({mode: "alwaysOn"}),
        }
    }
});

const inputCollection = st.s.getCollection("reshardingDb.coll");

CreateShardedCollectionUtil.shardCollectionWithChunks(inputCollection, {oldKey: 1}, [
    {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: st.shard0.shardName},
    {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: st.shard1.shardName},
]);

const inputCollectionUUID =
    getUUIDFromListCollections(inputCollection.getDB(), inputCollection.getName());
const inputCollectionUUIDString = extractUUIDFromObject(inputCollectionUUID);

const temporaryReshardingCollection =
    st.s.getCollection(`reshardingDb.system.resharding.${inputCollectionUUIDString}`);

assert.commandWorked(inputCollection.insert(
    [
        {_id: "stays on shard0", oldKey: -10, newKey: -10},
        {_id: "moves to shard0", oldKey: 10, newKey: -10},
        {_id: "moves to shard1", oldKey: -10, newKey: 10},
        {_id: "stays on shard1", oldKey: 10, newKey: 10},
    ],
    {writeConcern: {w: "majority"}}));

assert.commandWorked(st.s.adminCommand({
    reshardCollection: inputCollection.getFullName(),
    key: {newKey: 1},
    _presetReshardedChunks: [
        {min: {newKey: MinKey}, max: {newKey: 0}, recipientShardId: st.shard0.shardName},
        {min: {newKey: 0}, max: {newKey: MaxKey}, recipientShardId: st.shard1.shardName},
    ],
}));

function assertClonedContents(shard, expectedDocs) {
    // We sort by oldKey so the order of `expectedDocs` can be deterministic.
    assert.eq(expectedDocs,
              shard.rs.getPrimary()
                  .getCollection(temporaryReshardingCollection.getFullName())
                  .find()
                  .sort({oldKey: 1})
                  .toArray());
}

assertClonedContents(st.shard0, [
    {_id: "stays on shard0", oldKey: -10, newKey: -10},
    {_id: "moves to shard0", oldKey: 10, newKey: -10},
]);

assertClonedContents(st.shard1, [
    {_id: "moves to shard1", oldKey: -10, newKey: 10},
    {_id: "stays on shard1", oldKey: 10, newKey: 10},
]);

st.stop();
})();
