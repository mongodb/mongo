/**
 * Regression test for SERVER-132058: Ensure on FCV upgrade the authoritative metadata cloning does
 * not target shards that have been removed after historically owning chunks. Otherwise the upgrade
 * hangs trying to send a command to a shard that no longer exists.
 */
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (lastLTSFCV !== "8.0") {
    jsTest.log.info("Skipping test because AuthoritativeShards is already enabled in lastLTS");
    quit();
}

const st = new ShardingTest({shards: 2, config: 1, rs: {nodes: 1}});
const db = st.s.getDB("testDb");
const coll = db.coll;

// Downgrade the FCV so the shards are not authoritative.
assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);

// Create a sharded collection with one chunk on each shard, with shard0 as the DB primary.
assert.commandWorked(
    st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}),
);
CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
    {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

// Move shard1's chunk back to shard0 (so shard1 is only a historical owner) then remove the shard.
assert.commandWorked(
    st.s.adminCommand({
        moveChunk: coll.getFullName(),
        find: {x: 5},
        to: st.shard0.shardName,
        _waitForDelete: true,
    }),
);

removeShard(st, st.shard1.shardName);

// The upgrade must not hang targeting the removed shard that historically owned a chunk.
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

st.stop();
