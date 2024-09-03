/**
 * Tests that the reshardingCollectionCloner is resilient to staleConfig errors.
 *
 * @tags: [
 *   uses_atclustertime,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({mongos: 1, config: 1, shards: 2, rs: {nodes: 1}});

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

const documents = [
    {_id: "a0", info: "stays on shard0", oldKey: -10, newKey: -100},
    {_id: "a1", info: "stays on shard0", oldKey: -11, newKey: -101},
    {_id: "a2", info: "stays on shard0", oldKey: -12, newKey: -102},
    {_id: "b", info: "shard1 moves to shard0", oldKey: 10, newKey: -10},
    {_id: "b1", info: "shard1 moves to shard0", oldKey: 11, newKey: -10},
    {_id: "b2", info: "shard1 moves to shard0", oldKey: 12, newKey: -10},
    {_id: "c", info: "stays on shard1", oldKey: -10, newKey: -110},
    {_id: "c2", info: "stays on shard1", oldKey: -20, newKey: -120},
];
assert.commandWorked(inputCollection.insert(documents));
const originalInsertsTs = inputCollection.getDB().getSession().getOperationTime();

for (const shard of [st.shard0, st.shard1]) {
    shard.rs.getPrimary()
        .getDB(inputCollection.getDB().getName())
        .getSession()
        .advanceClusterTime(inputCollection.getDB().getSession().getClusterTime());
}

const shard0Primary = st.shard0.rs.getPrimary();
const outerLoop = configureFailPoint(
    shard0Primary, "reshardingCollectionClonerPauseBeforeWriteNaturalOrder", {}, {skip: 1});

const staleConfigFP =
    configureFailPoint(shard0Primary, "reshardingCollectionClonerShouldFailWithStaleConfig");

const reshardShell =
    startParallelShell(funWithArgs(
                           (inputCollectionFullName,
                            inputCollectionUUID,
                            shardName,
                            atClusterTime,
                            tempCollectionFullName) => {
                               assert.commandWorked(db.adminCommand({
                                   testReshardCloneCollection: inputCollectionFullName,
                                   shardKey: {newKey: 1},
                                   uuid: inputCollectionUUID,
                                   shardId: shardName,
                                   atClusterTime: atClusterTime,
                                   outputNs: tempCollectionFullName,
                               }));
                           },
                           inputCollection.getFullName(),
                           inputCollectionUUID,
                           st.shard0.shardName,
                           originalInsertsTs,
                           temporaryReshardingCollection.getFullName()),
                       shard0Primary.port);

// Wait until the retry of _writeOnceWithNaturalOrder to turn off the staleConfig fp.
outerLoop.wait();
staleConfigFP.off();
outerLoop.off();

reshardShell();

assert.eq(documents.length,
          st.s.getCollection(temporaryReshardingCollection.getFullName()).countDocuments({}));

// The temporary reshard collection must be dropped before checking metadata integrity.
assert(temporaryReshardingCollection.drop());

st.stop();
