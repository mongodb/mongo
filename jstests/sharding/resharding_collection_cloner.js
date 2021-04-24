/**
 * Tests the cloning portion of a resharding operation in isolation.
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
    rs: {nodes: 3},
    rsOptions: {
        setParameter: {
            "failpoint.WTPreserveSnapshotHistoryIndefinitely": tojson({mode: "alwaysOn"}),
            logComponentVerbosity: tojson({sharding: {reshard: 2}}),
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

CreateShardedCollectionUtil.shardCollectionWithChunks(temporaryReshardingCollection, {newKey: 1}, [
    {min: {newKey: MinKey}, max: {newKey: 0}, shard: st.shard0.shardName},
    {min: {newKey: 0}, max: {newKey: MaxKey}, shard: st.shard1.shardName},
]);

// The shardCollection command doesn't wait for the config.cache.chunks entries to have been written
// on the primary shard for the database. We manually run the _flushRoutingTableCacheUpdates command
// to guarantee they have been written and are visible with the atClusterTime used by the
// testReshardCloneCollection command.
for (const shard of [st.shard0, st.shard1]) {
    assert.commandWorked(shard.rs.getPrimary().adminCommand(
        {_flushRoutingTableCacheUpdates: temporaryReshardingCollection.getFullName()}));
}

assert.commandWorked(inputCollection.insert([
    {_id: "stays on shard0", oldKey: -10, newKey: -10},
    {_id: "moves to shard0", oldKey: 10, newKey: -10},
    {_id: "moves to shard1", oldKey: -10, newKey: 10},
    {_id: "stays on shard1", oldKey: 10, newKey: 10},
]));

const atClusterTime = inputCollection.getDB().getSession().getOperationTime();

assert.commandWorked(inputCollection.insert([
    {_id: "not visible, but would stay on shard0", oldKey: -10, newKey: -10},
    {_id: "not visible, but would move to shard0", oldKey: 10, newKey: -10},
    {_id: "not visible, but would move to shard1", oldKey: -10, newKey: 10},
    {_id: "not visible, but would stay on shard1", oldKey: 10, newKey: 10},
]));

// We wait for the "not visible" inserts to become majority-committed on all members of the replica
// set shards. This isn't necessary for the test's correctness but makes it more likely that the
// test would fail if ReshardingCollectionCloner wasn't specifying atClusterTime in its read
// concern.
st.shard0.rs.awaitLastOpCommitted();
st.shard1.rs.awaitLastOpCommitted();

function testReshardCloneCollection(shard, expectedDocs) {
    const dbName = inputCollection.getDB().getName();
    const allNodes = [...st.shard0.rs.nodes, ...st.shard1.rs.nodes];

    for (const node of allNodes) {
        node.getDB(dbName).setProfilingLevel(2);
    }

    const reshardCmd = {
        testReshardCloneCollection: inputCollection.getFullName(),
        shardKey: {newKey: 1},
        uuid: inputCollectionUUID,
        shardId: shard.shardName,
        atClusterTime: atClusterTime,
        outputNs: temporaryReshardingCollection.getFullName(),
    };
    jsTestLog({"Node": shard.rs.getPrimary(), "ReshardingCmd": reshardCmd});
    assert.commandWorked(shard.rs.getPrimary().adminCommand(reshardCmd));

    for (const node of allNodes) {
        node.getDB(dbName).setProfilingLevel(0);
    }

    // We sort by oldKey so the order of `expectedDocs` can be deterministic.
    assert.eq(expectedDocs,
              shard.rs.getPrimary()
                  .getCollection(temporaryReshardingCollection.getFullName())
                  .find()
                  .sort({oldKey: 1})
                  .toArray());

    // Verify the ReshardingCollectionCloner is sending its aggregation requests with a logical
    // session ID to prevent idle cursors from being timed out by the CursorManager.
    for (const donorShard of [st.shard0, st.shard1]) {
        const profilerEntries = donorShard.rs.nodes
                                    .map(node => node.getDB(dbName).system.profile.findOne({
                                        op: "command",
                                        ns: inputCollection.getFullName(),
                                        "command.aggregate": inputCollection.getName(),
                                        "command.collectionUUID": inputCollectionUUID,
                                    }))
                                    .filter(entry => entry !== null);

        assert.neq([],
                   profilerEntries,
                   `expected to find collection cloning aggregation in profiler output of some ${
                       donorShard.shardName} node`);

        for (const entry of profilerEntries) {
            assert(entry.command.hasOwnProperty("lsid"),
                   "expected profiler entry for collection cloning aggregation to have a logical" +
                       ` session ID: ${tojson(entry)}`);
        }
    }
}

testReshardCloneCollection(st.shard0, [
    {_id: "stays on shard0", oldKey: -10, newKey: -10},
    {_id: "moves to shard0", oldKey: 10, newKey: -10},
]);

testReshardCloneCollection(st.shard1, [
    {_id: "moves to shard1", oldKey: -10, newKey: 10},
    {_id: "stays on shard1", oldKey: 10, newKey: 10},
]);

st.stop();
})();
