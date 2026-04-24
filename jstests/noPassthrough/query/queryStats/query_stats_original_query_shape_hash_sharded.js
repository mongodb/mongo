/**
 * Tests that 'originalQueryShapeHash' in the shard-side query stats key correctly distinguishes
 * two aggregate pipelines that differ only in the router-side (merger) stage, when run against a
 * sharded collection that spans two shards.
 *
 * The two pipelines share the same shard-side fragment ($match + $group), so each shard records
 * the same query shape but two distinct 'originalQueryShapeHash' values — one per distinct
 * router-level pipeline. Each shard-side 'originalQueryShapeHash' must equal the router's own
 * 'queryShapeHash' for the corresponding full pipeline.
 *
 * Setup:
 *   - Collection sharded on { x: 1 }, split at x: 0.
 *   - Chunk (-∞, 0) stays on shard0; chunk [0, +∞) is moved to shard1.
 *   - Filter { x: { $lt: 1 } } spans both chunks, so both shards are targeted for every query.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {
    getQueryStats,
    getQueryStatsServerParameters,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "sharded_coll";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {nodes: 1},
    mongosOptions: getQueryStatsServerParameters(),
    rsOptions: getQueryStatsServerParameters(),
});

const mongos = st.s;
const mongosDB = mongos.getDB(dbName);
const shard0 = st.shard0;
const shard1 = st.shard1;

// Enable sharding with shard0 as the database primary.
assert.commandWorked(mongosDB.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));

const coll = mongosDB[collName];

// Shard on { x: 1 }, split at x: 0, move [0, +∞) to shard1.
assert.commandWorked(mongosDB.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
assert.commandWorked(mongosDB.adminCommand({split: coll.getFullName(), middle: {x: 0}}));
assert.commandWorked(mongosDB.adminCommand({moveChunk: coll.getFullName(), find: {x: 1}, to: shard1.shardName}));

// Insert documents: x=-5 lands in (-∞, 0) on shard0; x=5 lands in [0, +∞) on shard1.
assert.commandWorked(
    coll.insertMany([
        {x: -5, y: "CA", z: 1},
        {x: -5, y: "NY", z: 2},
        {x: 5, y: "CA", z: 3},
        {x: 5, y: "NY", z: 4},
    ]),
);

// Reset query stats on all three nodes so prior setup commands don't pollute results.
resetQueryStatsStore(mongos, "1MB");
resetQueryStatsStore(shard0, "1MB");
resetQueryStatsStore(shard1, "1MB");

// Run two aggregations whose full pipelines differ only in the $project field name.
// The shard-side fragment ($match + $group) is identical for both, so each shard records the
// same query shape but must store a distinct originalQueryShapeHash per router-level pipeline.
//
// Both queries use { x: { $lt: 1 } }, which spans both chunks, so mongos scatter-gathers to
// both shard0 and shard1.
assert.eq(
    coll
        .aggregate([
            {$match: {x: {$lt: 1}}},
            {$group: {_id: "$y", total: {$sum: "$z"}}},
            {$project: {_id: 1, mySum: "$total"}},
        ])
        .toArray().length,
    2,
);

assert.eq(
    coll
        .aggregate([
            {$match: {x: {$lt: 1}}},
            {$group: {_id: "$y", total: {$sum: "$z"}}},
            {$project: {_id: 1, hisSum: "$total"}},
        ])
        .toArray().length,
    2,
);

// -----------------------------------------------------------------------
// Collect query stats entries from all three nodes.
// -----------------------------------------------------------------------
const queryStatsOptions = {collName, customSort: {"key.originalQueryShapeHash": 1}};
const routerEntries = getQueryStats(mongos, queryStatsOptions);
const shard0Entries = getQueryStats(shard0, queryStatsOptions);
const shard1Entries = getQueryStats(shard1, queryStatsOptions);

// -----------------------------------------------------------------------
// Router: two entries, one per distinct full pipeline shape.
// -----------------------------------------------------------------------
assert.eq(
    routerEntries.length,
    2,
    "Expected 2 router query stats entries (one per pipeline): " + tojson(routerEntries),
);

const routerHashes = new Set(routerEntries.map((e) => e.queryShapeHash));
assert.eq(
    routerHashes.size,
    2,
    "Router entries should have two distinct queryShapeHash values: " + tojson(routerEntries),
);

// -----------------------------------------------------------------------
// Shard0 and shard1: two entries each — same shard-side query shape, different
// originalQueryShapeHash. Returns the set of originalQueryShapeHash values for further comparison.
// -----------------------------------------------------------------------
function assertShardEntries(shardName, entries, routerHashes, routerEntries) {
    assert.eq(
        entries.length,
        2,
        `Expected 2 ${shardName} query stats entries (one per originalQueryShapeHash): ` + tojson(entries),
    );

    // Both entries share the same shard-level queryShapeHash because the shard-side pipeline
    // ($match + partial $group) is identical for both router queries.
    assert.eq(
        entries[0].queryShapeHash,
        entries[1].queryShapeHash,
        `${shardName} entries should share the same shard-side queryShapeHash: ` + tojson(entries),
    );

    // The two originalQueryShapeHash values must be distinct.
    assert.neq(
        entries[0].key.originalQueryShapeHash,
        entries[1].key.originalQueryShapeHash,
        `${shardName} entries should have distinct originalQueryShapeHash values: ` + tojson(entries),
    );

    // Each originalQueryShapeHash must match one of the router's queryShapeHash values.
    const originalHashes = new Set(entries.map((e) => e.key.originalQueryShapeHash));
    for (const hash of originalHashes) {
        assert(
            routerHashes.has(hash),
            `${shardName} originalQueryShapeHash ${hash}` +
                " not found among router queryShapeHash values: " +
                tojson(routerEntries),
        );
    }
    return originalHashes;
}

const shard0OriginalHashes = assertShardEntries("shard0", shard0Entries, routerHashes, routerEntries);
const shard1OriginalHashes = assertShardEntries("shard1", shard1Entries, routerHashes, routerEntries);

// Both shards must agree on which originalQueryShapeHash maps to which router query.
assert.sameMembers(
    [...shard0OriginalHashes],
    [...shard1OriginalHashes],
    "Shard0 and shard1 should record the same set of originalQueryShapeHash values",
);

st.stop();
