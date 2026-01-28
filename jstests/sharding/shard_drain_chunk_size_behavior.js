/*
 * Shard a collection with documents spread on 2 shards and then start draining checking that:
 * - Huge non-jumbo chunks are split during draining (moveRange moves off pieces of `chunkSize` MB)
 * - Unmarked jumbo chunks are marked as jumbo and NOT moved during draining, requiring manual move with `forceJumbo: true`
 *
 * Regression test for SERVER-76550.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({
    other: {
        enableBalancer: false,
        chunkSize: 1,
        configOptions: {setParameter: {balancerMigrationsThrottlingMs: 100}},
    },
});

const mongos = st.s0;
const configDB = st.getDB("config");

// Stop auto-merger because the test expects a specific number of chunks
sh.stopAutoMerger(configDB);

const dbName = "test";
const collName = "collToDrain";
const ns = dbName + "." + collName;
const db = st.getDB(dbName);
const coll = db.getCollection(collName);

// Shard collection with shard0 as db primary.
assert.commandWorked(mongos.adminCommand({enablesharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {x: 1}}));

// shard0 owns docs with shard key [MinKey, 0), shard1 owns docs with shard key [0, MaxKey).
assert.commandWorked(st.s.adminCommand({moveRange: ns, min: {x: 0}, max: {x: MaxKey}, toShard: st.shard1.shardName}));

// Insert ~20MB of docs with different shard keys (10MB on shard0 and 10MB on shard1)
// and ~10MB of docs with the same shard key (jumbo chunk).
const big = "X".repeat(1024 * 1024); // 1MB
const jumboKey = 100;
let bulk = coll.initializeUnorderedBulkOp();
for (let i = -10; i < 10; i++) {
    bulk.insert({x: i, big: big});
    bulk.insert({x: jumboKey, big: big});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 10}}));

// Check that there are 3 chunks before starting draining.
const chunksBeforeDrain = findChunksUtil.findChunksByNs(configDB, ns).toArray();
assert.eq(3, chunksBeforeDrain.length);

st.startBalancer();

// Start draining shard1 and wait for it to drain all non-jumbo chunks.
assert.commandWorked(mongos.adminCommand({startShardDraining: st.shard1.shardName}));
assert.soon(
    () => {
        try {
            const statusRes = assert.commandWorked(mongos.adminCommand({shardDrainingStatus: st.shard1.shardName}));

            // Check if remaining chunks on shard1 are marked as jumbo
            const chunksOnShard1 = findChunksUtil.findChunksByNs(configDB, ns, {shard: st.shard1.shardName}).toArray();
            const allJumbo = chunksOnShard1.every((chunk) => chunk.jumbo === true);

            jsTest.log(`Draining status: ${statusRes.state}, chunks remaining on shard1: ${chunksOnShard1}`);
            return allJumbo;
        } catch (e) {
            jsTest.log("Transient error checking draining status: " + e.message);
            return false;
        }
    },
    "Timed out waiting for all non-jumbo chunks to be drained",
    5 * 60 * 1000, // 5 minute timeout
);

st.stopBalancer();

const chunksOnShard0AfterDrain = findChunksUtil.findChunksByNs(configDB, ns, {shard: st.shard0.shardName}).toArray();
const chunksOnShard1AfterDrain = findChunksUtil.findChunksByNs(configDB, ns, {shard: st.shard1.shardName}).toArray();

// Expect 11 chunks on shard0:
// - [MinKey, 0)                   original chunk on shard0
// - [0, 1), [1, 2), ... [9, 10)   10 chunks split from [0, 10) during draining
assert.eq(11, chunksOnShard0AfterDrain.length, "Expected non-jumbo chunks migrated to shard0");

// Expect 1 jumbo chunk remaining on shard1: [10, MaxKey).
assert.eq(1, chunksOnShard1AfterDrain.length, "Expected jumbo chunk still on shard1");

jsTest.log("Manually moving jumbo chunk with forceJumbo: true");
assert.commandWorked(
    mongos.adminCommand({
        moveChunk: ns,
        find: {x: jumboKey},
        to: st.shard0.shardName,
        forceJumbo: true,
        _waitForDelete: true,
    }),
);

const finalChunksOnShard0 = findChunksUtil.findChunksByNs(configDB, ns, {shard: st.shard0.shardName}).toArray();
const finalChunksOnShard1 = findChunksUtil.findChunksByNs(configDB, ns, {shard: st.shard1.shardName}).toArray();
assert.eq(12, finalChunksOnShard0.length, "All chunks should be on shard0 after manual move");
assert.eq(0, finalChunksOnShard1.length, "No chunks should remain on shard1");

// Complete the shard removal operation now that all chunks have been moved.
assert.commandWorked(mongos.adminCommand({commitShardRemoval: st.shard1.shardName}));

st.stop();
