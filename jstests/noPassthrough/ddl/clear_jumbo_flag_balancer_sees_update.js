/**
 * Verifies that the balancer's routing cache observes the cleared jumbo flag immediately after
 * clearJumboFlag, without requiring a placement-version bump.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_90,
 * ]
 */

// Direct writes to config.chunks (used to stamp the jumbo flag) bypass shard filtering metadata
// consistency checks.
TestData.skipCheckShardFilteringMetadata = true;

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({
    shards: 2,
    other: {
        chunkSize: 1,
        enableBalancer: false,
    },
});

const dbName = jsTestName();
const collName = "coll";
const ns = dbName + "." + collName;

const adminDB = st.s.getDB("admin");
const configDB = st.s.getDB("config");
// Log messages from the balancer are emitted on the config server primary.
const csrsPrimary = st.configRS.getPrimary();

// --------------------------------------------------------------------------
// Setup:
// There are two shards. Zone "Z" is assigned only to shard1 and covers the range [0, MaxKey).
// The chunk [0, MaxKey) is assigned to zone "Z", but it initially resides on shard0, creating a
// zone violation.
// --------------------------------------------------------------------------

assert.commandWorked(
    adminDB.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(adminDB.runCommand({addShardToZone: st.shard1.shardName, zone: "Z"}));
assert.commandWorked(adminDB.runCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(adminDB.runCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(
    adminDB.runCommand({updateZoneKeyRange: ns, min: {x: 0}, max: {x: MaxKey}, zone: "Z"}),
);

// Move [0, MaxKey) to shard0 if it was placed elsewhere during sharding.
const chunkBeforeSetup = findChunksUtil.findOneChunkByNs(configDB, ns, {min: {x: 0}});
assert(chunkBeforeSetup, "expected chunk with min {x:0}");
if (chunkBeforeSetup.shard !== st.shard0.shardName) {
    assert.commandWorked(
        adminDB.runCommand({
            moveRange: ns,
            min: {x: 0},
            max: {x: MaxKey},
            toShard: st.shard0.shardName,
        }),
    );
}

// Stamp the chunk as jumbo directly on config.chunks — the balancer will see it as unmovable
// until clearJumboFlag clears it.
assert.commandWorked(
    configDB.chunks.updateOne({uuid: chunkBeforeSetup.uuid, min: {x: 0}}, {$set: {jumbo: true}}),
);
const jumboChunkBefore = findChunksUtil.findOneChunkByNs(configDB, ns, {min: {x: 0}});
assert(jumboChunkBefore.jumbo, "jumbo flag not set on config.chunks");

st.forEachConfigServer((conn) =>
    assert.commandWorked(conn.adminCommand({setParameter: 1, balancerMigrationsThrottlingMs: 0})),
);

// --------------------------------------------------------------------------
// Round 1: the balancer must skip the chunk because it is jumbo and emit
// log 21891 ("Chunk violates zone, but it is jumbo and cannot be moved").
// --------------------------------------------------------------------------

st.startBalancer();
st.awaitBalancerRound();

const chunkAfterRound1 = findChunksUtil.findOneChunkByNs(configDB, ns, {min: {x: 0}});
assert.eq(
    st.shard0.shardName,
    chunkAfterRound1.shard,
    "jumbo chunk should not have moved during round 1",
);

// Confirm the balancer logged the jumbo-skip warning.
checkLog.containsJson(csrsPrimary, 21891, {namespace: ns});

jsTestLog("Round 1 complete: balancer correctly skipped the jumbo chunk (log 21891 confirmed).");

// --------------------------------------------------------------------------
// clearJumboFlag: mutates the in-memory ChunkInfo and clears the flag on disk.
// The placement version must NOT be bumped.
// --------------------------------------------------------------------------

assert.commandWorked(adminDB.runCommand({clearJumboFlag: ns, find: {x: 0}}));

const chunkAfterClear = findChunksUtil.findOneChunkByNs(configDB, ns, {min: {x: 0}});
assert(!chunkAfterClear.jumbo, "jumbo flag should be cleared on disk after clearJumboFlag");
assert.eq(
    jumboChunkBefore.lastmod.getTime(),
    chunkAfterClear.lastmod.getTime(),
    "clearJumboFlag must not bump the placement version",
);

jsTestLog("clearJumboFlag done: disk cleared, version unchanged.");

// --------------------------------------------------------------------------
// Round 2: the balancer reads the cleared flag from the in-memory routing
// cache and migrates the chunk to shard1 in this round.
// --------------------------------------------------------------------------

assert.soon(() => {
    const chunk = findChunksUtil.findOneChunkByNs(configDB, ns, {min: {x: 0}});
    return chunk && chunk.shard === st.shard1.shardName;
}, "balancer did not migrate the chunk to shard1 after clearJumboFlag; " + "the in-memory routing-cache update may not be working");
st.stopBalancer();

jsTestLog(
    "Round 2 complete: balancer migrated the chunk to shard1 immediately after clearJumboFlag.",
);

st.stop();
