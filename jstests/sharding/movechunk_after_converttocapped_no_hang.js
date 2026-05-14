/**
 * Regression test for the moveChunk hang observed after a convertToCapped operation on a sharded
 * collection.
 *
 * Background: when convertToCapped runs against a sharded collection, the post-step on the data
 * shard previously left the collection's filtering metadata and migration coordinator state in an
 * inconsistent shape. A subsequent moveChunk would then never make progress on the source shard:
 * the session-migration scan over kGroupForPossiblyRetryableOperations would loop forever (the
 * surface symptom users see is repeated "moveChunk data transfer progress" log lines with no
 * completion).
 *
 * This test exercises the user-visible path:
 *
 *   1. Create a sharded collection with a single chunk on the primary shard.
 *   2. Run convertToCapped on the collection.
 *   3. Run moveChunk with a bounded maxTimeMS so the test cannot hang the suite even on
 *      unpatched binaries.
 *   4. Assert moveChunk completes successfully and the chunk count on each shard reflects the
 *      migration.
 *
 * On unpatched code paths this test times out at moveChunk and fails with MaxTimeMSExpired (or the
 * chunk-location assertion below, depending on which side wins the race). On a patched build the
 * moveChunk completes well under the bounded timeout.
 *
 * @tags: [
 *  requires_sharding,
 *  requires_fcv_90,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 1},
});

const dbName = jsTestName();
const collName = "convertToCappedThenMove";
const ns = dbName + "." + collName;

const testDB = st.s.getDB(dbName);
const coll = testDB.getCollection(collName);

// Pin the database's primary shard so we know which side owns the only chunk before the move.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Seed a small amount of data so the convertToCapped path has documents to copy and the
// subsequent moveChunk actually transfers a non-empty chunk.
const seedBatch = [];
for (let i = 0; i < 64; ++i) {
    seedBatch.push({_id: i, payload: "x".repeat(32)});
}
assert.commandWorked(coll.insert(seedBatch));

assert.eq(64, coll.countDocuments({}));
assert(!coll.isCapped(), "collection should not be capped prior to convertToCapped");

jsTestLog("Running convertToCapped on the sharded collection.");
assert.commandWorked(testDB.runCommand({convertToCapped: collName, size: 1024 * 1024}));
assert(coll.isCapped(), "convertToCapped did not capped the collection");

// Sanity check that one chunk exists on the primary shard before the move.
const initialChunks = findChunksUtil.findChunksByNs(st.config, ns).toArray();
assert.eq(1, initialChunks.length, "expected exactly one chunk before moveChunk");
assert.eq(st.shard0.shardName, initialChunks[0].shard);

// Bounded timeout. On unpatched code moveChunk never makes progress past session migration; the
// bound below ensures the test fails loudly instead of hanging the suite. 60s is generous enough
// that a healthy moveChunk on a tiny chunk in a two-node ShardingTest completes comfortably.
const moveChunkTimeoutMs = 60 * 1000;

jsTestLog("Running moveChunk with bounded maxTimeMS=" + moveChunkTimeoutMs);
const moveChunkResult = st.s.adminCommand({
    moveChunk: ns,
    find: {_id: 0},
    to: st.shard1.shardName,
    maxTimeMS: moveChunkTimeoutMs,
});

// On unpatched binaries this assertion fails with MaxTimeMSExpired. On a patched build, the
// migration completes well within the bounded timeout.
assert.commandWorked(
    moveChunkResult,
    "moveChunk after convertToCapped did not complete within " + moveChunkTimeoutMs + "ms;" +
        " this is the SERVER-126352 hang signature.",
);

// Verify the chunk actually moved.
const numChunksOnShard0 =
    findChunksUtil.findChunksByNs(st.config, ns, {shard: st.shard0.shardName}).itcount();
const numChunksOnShard1 =
    findChunksUtil.findChunksByNs(st.config, ns, {shard: st.shard1.shardName}).itcount();
assert.eq(0, numChunksOnShard0, "expected zero chunks on shard0 after move");
assert.eq(1, numChunksOnShard1, "expected one chunk on shard1 after move");

st.stop();
