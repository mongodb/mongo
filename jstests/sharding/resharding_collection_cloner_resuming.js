/**
 * Tests the resuming behavior of resharding's collection cloning.
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
    },
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

CreateShardedCollectionUtil.shardCollectionWithChunks(
    temporaryReshardingCollection,
    {newKey: 1},
    [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: st.shard0.shardName}]);

// The shardCollection command doesn't wait for the config.cache.chunks entries to have been written
// on the primary shard for the database. We manually run the _flushRoutingTableCacheUpdates command
// to guarantee they have been written and are visible with the atClusterTime used by the
// testReshardCloneCollection command.
for (const shard of [st.shard0, st.shard1]) {
    assert.commandWorked(shard.rs.getPrimary().adminCommand(
        {_flushRoutingTableCacheUpdates: temporaryReshardingCollection.getFullName()}));
}

const documents = [
    {_id: "a", info: "stays on shard0", oldKey: -10, newKey: 0},
    {_id: "b", info: "moves to shard0", oldKey: 10, newKey: 0},
    {_id: "c", info: "stays on shard0", oldKey: -10, newKey: 0},
    {_id: "d", info: "moves to shard0", oldKey: 10, newKey: 0},
];
assert.commandWorked(inputCollection.insert(documents));
const originalInsertsTs = inputCollection.getDB().getSession().getOperationTime();

function testReshardCloneCollection(shard, expectedDocs, atClusterTime) {
    const shardPrimary = shard.rs.getPrimary();
    shardPrimary.getDB(inputCollection.getDB().getName())
        .getSession()
        .advanceClusterTime(inputCollection.getDB().getSession().getClusterTime());

    assert.commandWorked(shardPrimary.adminCommand({
        testReshardCloneCollection: inputCollection.getFullName(),
        shardKey: {newKey: 1},
        uuid: inputCollectionUUID,
        shardId: shard.shardName,
        atClusterTime: atClusterTime,
        outputNs: temporaryReshardingCollection.getFullName(),
    }));

    // We sort by _id so the order of `expectedDocs` can be deterministic.
    assert.eq(expectedDocs,
              shardPrimary.getCollection(temporaryReshardingCollection.getFullName())
                  .find()
                  .sort({_id: 1})
                  .toArray());
}

testReshardCloneCollection(st.shard0, documents, originalInsertsTs);

// Cloning the sharded collection a second time should be a successful no-op.
testReshardCloneCollection(st.shard0, documents, originalInsertsTs);

// Removing the "c" and "d" documents from the temporary resharding collection to simulate the
// cloner as having made partial progress. It should successfully resume from the "b" document.
assert.commandWorked(temporaryReshardingCollection.remove({_id: {$gt: "b"}}, {justOne: false}));
testReshardCloneCollection(st.shard0, documents, originalInsertsTs);

// Insert another "d" document and verify that resuming now fails due to a duplicate key error.
assert.commandWorked(
    inputCollection.insert({_id: "d", info: "stays on shard0", oldKey: -10, newKey: 0}));

const duplicateInsertTs = inputCollection.getDB().getSession().getOperationTime();

assert.commandFailedWithCode(st.shard0.adminCommand({
    testReshardCloneCollection: inputCollection.getFullName(),
    shardKey: {newKey: 1},
    uuid: inputCollectionUUID,
    shardId: st.shard0.shardName,
    atClusterTime: duplicateInsertTs,
    outputNs: temporaryReshardingCollection.getFullName(),
}),
                             ErrorCodes.DuplicateKey);

st.stop();
})();
