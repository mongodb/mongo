/**
 * Tests the resuming behavior of resharding's collection cloning.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   uses_atclustertime,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 4,
    rs: {nodes: 1},
    rsOptions: {
        setParameter: {
            "failpoint.WTPreserveSnapshotHistoryIndefinitely": tojson({mode: "alwaysOn"}),
        }
    },
});

if (!FeatureFlagUtil.isEnabled(st.s, "ReshardingImprovements")) {
    jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled");
    st.stop();
    quit();
}

const inputCollection = st.s.getCollection("reshardingDb.coll");
// Padding sufficient to ensure that only one document can appear per batch.
const bigString = 'a'.repeat(1024 * 1024 * 9);

CreateShardedCollectionUtil.shardCollectionWithChunks(inputCollection, {oldKey: 1}, [
    {min: {oldKey: MinKey}, max: {oldKey: -9}, shard: st.shard0.shardName},
    {min: {oldKey: -9}, max: {oldKey: 9}, shard: st.shard1.shardName},
    {min: {oldKey: 9}, max: {oldKey: 10000}, shard: st.shard2.shardName},
    {min: {oldKey: 10000}, max: {oldKey: MaxKey}, shard: st.shard3.shardName},
]);

const inputCollectionUUID =
    getUUIDFromListCollections(inputCollection.getDB(), inputCollection.getName());
const inputCollectionUUIDString = extractUUIDFromObject(inputCollectionUUID);

const temporaryReshardingCollection =
    st.s.getCollection(`reshardingDb.system.resharding.${inputCollectionUUIDString}`);

CreateShardedCollectionUtil.shardCollectionWithChunks(temporaryReshardingCollection, {newKey: 1}, [
    {min: {newKey: MinKey}, max: {newKey: -9}, shard: st.shard0.shardName},
    {min: {newKey: -9}, max: {newKey: 9}, shard: st.shard1.shardName},
    {min: {newKey: 9}, max: {newKey: 10000}, shard: st.shard2.shardName},
    {min: {newKey: 10000}, max: {newKey: MaxKey}, shard: st.shard3.shardName},
]);

// The shardCollection command doesn't wait for the config.cache.chunks entries to have been written
// on the primary shard for the database. We manually run the _flushRoutingTableCacheUpdates command
// to guarantee they have been written and are visible with the atClusterTime used by the
// testReshardCloneCollection command.
for (const shard of [st.shard0, st.shard1]) {
    assert.commandWorked(shard.rs.getPrimary().adminCommand(
        {_flushRoutingTableCacheUpdates: temporaryReshardingCollection.getFullName()}));
}

// Shard 3 intentionally starts with no data.
// Shard 1 starts with only one small document to maximize the chance it will be finished before
// the failpoint to force resume is hit.
const documents = [
    {_id: "a0", info: "stays on shard0", oldKey: -10, newKey: -100, padding: bigString},
    {_id: "a1", info: "stays on shard0", oldKey: -10, newKey: -101, padding: bigString},
    {_id: "a2", info: "stays on shard0", oldKey: -10, newKey: -102, padding: bigString},
    {_id: "a3", info: "stays on shard0", oldKey: -10, newKey: -103, padding: bigString},
    {_id: "a4", info: "stays on shard0", oldKey: -10, newKey: -104, padding: bigString},
    {_id: "a5", info: "stays on shard0", oldKey: -10, newKey: -105, padding: bigString},
    {_id: "b", info: "shard1 moves to shard0", oldKey: 0, newKey: -10},
    {_id: "c0", info: "shard2 moves to shard0", oldKey: 10, newKey: -200, padding: bigString},
    {_id: "c1", info: "shard2 moves to shard0", oldKey: 10, newKey: -201, padding: bigString},
    {_id: "c2", info: "shard2 moves to shard0", oldKey: 10, newKey: -202, padding: bigString},
    {_id: "c3", info: "shard2 moves to shard0", oldKey: 10, newKey: -203, padding: bigString},
    {_id: "c4", info: "shard2 moves to shard0", oldKey: 10, newKey: -204, padding: bigString},
    // Docs d and e should not appear.
    {_id: "d", info: "shard0 moves to shard1", oldKey: -10, newKey: 0},
    {_id: "e", info: "shard2 moves to shard3", oldKey: 10, newKey: 20000},
];
assert.commandWorked(inputCollection.insert(documents));
const originalInsertsTs = inputCollection.getDB().getSession().getOperationTime();

// This is the destination shard we'll be forcing to restart.
const shard0Primary = st.shard0.rs.getPrimary();
assert.commandWorked(shard0Primary.adminCommand(
    {"setParameter": 1, logComponentVerbosity: {sharding: {reshard: 3}}}));

// Allow several reads to go through before aborting.
assert.commandWorked(shard0Primary.adminCommand({
    "configureFailPoint": "reshardingCollectionClonerAbort",
    mode: {"skip": 2},
    data: {"donorShard": st.shard0.shardName}
}));

const attemptFp = configureFailPoint(
    shard0Primary, "reshardingCollectionClonerPauseBeforeAttempt", {}, {"skip": 1});

shard0Primary.getDB(inputCollection.getDB().getName())
    .getSession()
    .advanceClusterTime(inputCollection.getDB().getSession().getClusterTime());

jsTestLog("About to start resharding, first attempt");
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
// Wait for the first attempt to fail.
attemptFp.wait();

// Turn off the abort failpoint so the second attempt succeeds.
assert.commandWorked(shard0Primary.adminCommand({
    "configureFailPoint": "reshardingCollectionClonerAbort",
    mode: "off",
    data: {"donorShard": st.shard0.shardName}
}));
jsTestLog("About to start resharding, second attempt");
attemptFp.off();
reshardShell();

// Delete the padding to make test failures more readable.
for (let i = 0; i < documents.length; i++) {
    delete documents[i].padding;
}
let expectedDocs = documents.filter((doc) => doc.newKey < -9);
// We sort by _id so the order of `expectedDocs` can be deterministic.
assert.eq(expectedDocs,
          shard0Primary.getCollection(temporaryReshardingCollection.getFullName())
              .find({}, {padding: 0})
              .sort({_id: 1})
              .toArray());

// The temporary reshard collection must be dropped before checking metadata integrity.
assert(temporaryReshardingCollection.drop());

st.stop();
