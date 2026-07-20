/**
 * Regression test for SERVER-131694: Ensure on FCV upgrade the authoritative metadata is cloned to
 * all shards that historically owned a chunk, not just current owners. Otherwise a point-in-time
 * read routed to such a historical owner can't filter orphans during snapshot reads.
 * @tags: [requires_persistence]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (lastLTSFCV !== "8.0") {
    jsTest.log.info("Skipping test because AuthoritativeShards is already enabled in lastLTS");
    quit();
}

const st = new ShardingTest({shards: 3, config: 1, rs: {nodes: 1}});
const db = st.s.getDB("testDb");
const coll = db.sharded;
const ns = coll.getFullName();

// Downgrade the FCV so the shards are not authoritative.
assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);

// Create a sharded collection with 2 chunks, with shard2 as the DB primary.
assert.commandWorked(
    st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard2.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));

// [0, MaxKey) passes through shard1 then goes to shard0, leaving an doc on shard1 pending deletion.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 5}, to: st.shard1.shardName}));
assert.commandWorked(coll.insertOne({x: 5}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 5}, to: st.shard0.shardName}));

// [MinKey, 0) is owned by shard1 at snapshot time, then it's moved and shard1 owns no chunks.
const clusterTime = assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: {x: -1}, to: st.shard1.shardName}),
).operationTime;
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: -1}, to: st.shard0.shardName}));

// Make shards authoritative:
// Even though shard1 currently owns no chunks, it must clone the historical information.
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

// Verify shard1 has the authoritative metadata.
const entries = st.shard1
    .getDB("config")
    .shard.catalog.collections.find({_id: coll.getFullName()})
    .toArray();
assert.eq(1, entries.length, "expected to find collection entry on shard1", {entries});

// Restart shard1 so the read must rebuild the orphan filter from the authoritative metadata.
st.restartShardRS(1, true);

// Run a snapshot read at a time where both shard0 and shard1 owned chunks, so it needs a broadcast.
const docs = coll.find().readConcern("snapshot", clusterTime).toArray();
assert.eq(
    [5],
    docs.map((d) => d.x),
    "Unexpected documents",
    {docs},
);

st.stop();
