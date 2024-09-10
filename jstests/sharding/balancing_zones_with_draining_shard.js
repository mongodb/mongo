/**
 * Test that chunks and documents are moved correctly among zones with a draining shard.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertChunksOnShards,
    assertDocsOnShards,
    assertShardTags,
    runBalancer,
} from "jstests/sharding/libs/zone_changes_util.js";

const st = new ShardingTest({shards: 3, other: {chunkSize: 1 /* 1MB */}});
let primaryShard = st.shard0;
let dbName = "test";
let testDB = st.s.getDB(dbName);
let configDB = st.s.getDB("config");
let coll = testDB.coll;
let ns = coll.getFullName();
let shardKey = {x: 1};

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard.shardName}));

jsTest.log("Shard the collection and create chunks.");
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -10}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 10}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 20}}));

const bigString = 'X'.repeat(1024 * 1024);  // 1MB
jsTest.log("Insert docs (one for each chunk) and check that they end up on the primary shard.");
let docs = [
    {x: -15, s: bigString},
    {x: -5, s: bigString},
    {x: 5, s: bigString},
    {x: 15, s: bigString},
    {x: 25, s: bigString}
];
assert.commandWorked(coll.insert(docs));

jsTest.log("Check the docs are on shard0.");
assert.eq(docs.length, primaryShard.getCollection(ns).count());

jsTest.log("Remove shard0 to set it in draining mode");
const removeShardFp =
    configureFailPoint(st.configRS.getPrimary(), "hangRemoveShardAfterSettingDrainingFlag");
const awaitRemoveShard = startParallelShell(
    funWithArgs(async function(shardToRemove) {
        const {removeShard} = await import("jstests/sharding/libs/remove_shard_util.js");
        removeShard(db, shardToRemove);
    }, st.shard0.shardName), st.s.port);
removeShardFp.wait();

// Setup the following zone configuration:
//   - shard0 -> [zoneA]
//   - shard1 -> [zoneB]
//   - shard2 -> []
//
//   - zoneA: [MinKey, 10)
//   - zoneB: [10, MaxKey)
jsTest.log("Add shards to zones and assign zone key ranges.");
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zoneA"}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "zoneB"}));
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: ns, min: {x: MinKey}, max: {x: 10}, zone: "zoneA"}));
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: ns, min: {x: 10}, max: {x: MaxKey}, zone: "zoneB"}));

jsTest.log("Check that the shards have the assigned zones.");
let shardTags =
    {[st.shard0.shardName]: ["zoneA"], [st.shard1.shardName]: ["zoneB"], [st.shard2.shardName]: []};
assertShardTags(configDB, shardTags);

jsTest.log(
    "Let the balancer do the balancing, and check that the chunks and the docs are on the right shards.");
{
    runBalancer(st, 2);
    const shardChunkBounds = {
        [st.shard0.shardName]: [[{x: MinKey}, {x: -10}], [{x: -10}, {x: 0}], [{x: 0}, {x: 10}]],
        [st.shard1.shardName]: [[{x: 10}, {x: 20}], [{x: 20}, {x: MaxKey}]],
        [st.shard2.shardName]: []
    };
    assertChunksOnShards(configDB, ns, shardChunkBounds);
    assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);
    assert.eq(docs.length, coll.countDocuments({}));
}

jsTest.log(
    "Add shard2 to zoneA to be able to move all the chunks out of the draining shard (shard0)");
//   - shard0 -> [zoneA]
//   - shard1 -> [zoneB]
//   - shard2 -> [zoneA]
//
//   - zoneA: [MinKey, 10)
//   - zoneB: [10, MaxKey)
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: "zoneA"}));

jsTest.log(
    "Let the balancer do the balancing, and check that the chunks and the docs are on the right shards.");
{
    runBalancer(st, 3);
    const shardChunkBounds = {
        [st.shard0.shardName]: [],
        [st.shard1.shardName]: [[{x: 10}, {x: 20}], [{x: 20}, {x: MaxKey}]],
        [st.shard2.shardName]: [[{x: MinKey}, {x: -10}], [{x: -10}, {x: 0}], [{x: 0}, {x: 10}]]
    };
    assertChunksOnShards(configDB, ns, shardChunkBounds);
    assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);
    assert.eq(docs.length, coll.countDocuments({}));
}

jsTest.log("Finally, wait for shard0 to be completely removed.");
st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName});
removeShardFp.off();
awaitRemoveShard();

st.stop();
