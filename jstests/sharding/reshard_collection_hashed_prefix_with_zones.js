/**
 * Tests that resharding to a shard key with a hashed prefix respects the placement requested by
 * reshardCollection, both when it is expressed as zones and as a shardDistribution.
 *
 * A hashed prefix lets resharding take a fast path that evenly distributes one chunk per shard.
 * That policy is neither zone nor shardDistribution aware, so it may only be used when the caller
 * has not asked for a specific placement.
 *
 * @tags: [
 *  requires_sharding,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({shards: 2});
const configDB = st.s.getDB("config");

const dbName = "testDb";
// The requested placements below split the hashed key space in two halves, one per shard.
const splitPoint = NumberLong(0);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

// Shards 'collName' on a non-hashed key and seeds it, so that resharding to a hashed-prefix key is
// a real change and the sampling based policies have documents to sample.
function createAndSeedCollection(collName) {
    const ns = dbName + "." + collName;
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

    const bulk = st.s.getDB(dbName).getCollection(collName).initializeUnorderedBulkOp();
    for (let i = 0; i < 1000; i++) {
        bulk.insert({oldKey: i, newKey: i});
    }
    assert.commandWorked(bulk.execute());
    return ns;
}

// Asserts that 'ns' has exactly one chunk per requested range, with the requested bounds and on the
// requested shard. 'expectedChunks' must be ordered by ascending 'min'.
function assertChunksAre(ns, expectedChunks) {
    const chunks = findChunksUtil
        .findChunksByNs(configDB, ns)
        .sort({min: 1})
        .toArray()
        .map((chunk) => ({min: chunk.min, max: chunk.max, shard: chunk.shard}));
    assert.docEq(expectedChunks, chunks, "resharding did not honor the requested placement");
}

jsTest.log.info("Resharding to a hashed-prefix shard key with zones.");

const zonedNs = createAndSeedCollection("zonedColl");
const zoneA = "zoneA";
const zoneB = "zoneB";
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: zoneA}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: zoneB}));

assert.commandWorked(
    st.s.adminCommand({
        reshardCollection: zonedNs,
        key: {newKey: "hashed"},
        numInitialChunks: 2,
        zones: [
            {zone: zoneA, min: {newKey: MinKey}, max: {newKey: splitPoint}},
            {zone: zoneB, min: {newKey: splitPoint}, max: {newKey: MaxKey}},
        ],
    }),
);

assertChunksAre(zonedNs, [
    {min: {newKey: MinKey}, max: {newKey: splitPoint}, shard: st.shard0.shardName},
    {min: {newKey: splitPoint}, max: {newKey: MaxKey}, shard: st.shard1.shardName},
]);

jsTest.log.info("Resharding to a hashed-prefix shard key with a shardDistribution.");

const distributionNs = createAndSeedCollection("shardDistributionColl");

// Deliberately the reverse of the zoned case, so a policy that ignores the request and round-robins
// the shards instead cannot pass by coincidence.
assert.commandWorked(
    st.s.adminCommand({
        reshardCollection: distributionNs,
        key: {newKey: "hashed"},
        numInitialChunks: 2,
        shardDistribution: [
            {shard: st.shard1.shardName, min: {newKey: MinKey}, max: {newKey: splitPoint}},
            {shard: st.shard0.shardName, min: {newKey: splitPoint}, max: {newKey: MaxKey}},
        ],
    }),
);

assertChunksAre(distributionNs, [
    {min: {newKey: MinKey}, max: {newKey: splitPoint}, shard: st.shard1.shardName},
    {min: {newKey: splitPoint}, max: {newKey: MaxKey}, shard: st.shard0.shardName},
]);

st.stop();
