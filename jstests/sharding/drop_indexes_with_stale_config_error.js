/*
 * Tests that a StaleConfigError received from a shard on dropIndexes allows the command to
 * successfully complete upon retry. A shard can never be ignorant of its own metadata under
 * authoritative shards, so the staleness is induced on a router instead:
 *
 *   - When dropIndexes is a versioned mongos fanout, the stale router is the mongos that issues it.
 *   - When dropIndexes runs as a DDL coordinator (featureFlagDropIndexesDDLCoordinator), the router
 *     that matters is the coordinator on the database's primary shard.
 *
 * The database primary is shard1 (a regular data-bearing shard) rather than shard0, so that the
 * dropIndexes coordinator -- which runs on the primary shard -- is hosted on a shard whose routing
 * cache can lag. In config-shard mode shard0 is the config server, whose coordinator resolves
 * routing authoritatively and is never stale; hosting the coordinator on shard1 keeps the scenario
 * reproducible there too.
 *
 * The setup below leaves both stale at once: shard1 (the primary and coordinator host) donates the
 * whole chunk to shard0 -- which primes shard1's routing cache to believe shard0 owns it -- and the
 * chunk is then moved shard0 -> shard2 through a different router, so neither shard1 nor the issuing
 * mongos learns of the final move.
 */

import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunks.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

const st = new ShardingTest({mongos: 2, shards: 3});

const dbName = jsTestName();
const collName = "coll";
const ns = dbName + "." + collName;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.getCollection(ns).createIndex({y: 1}));

// Donate the whole chunk from the primary shard1 to shard0. shard1 is the donor, so its routing
// cache (used by the dropIndexes coordinator that runs on the primary) is left believing shard0
// owns the chunk.
ChunkHelper.moveChunk(
    st.s1.getDB(dbName),
    collName,
    [{x: MinKey}, {x: MaxKey}],
    st.shard0.shardName,
    true /* waitForDelete */,
);

// Move the chunk shard0 -> shard2 through st.s1 and drop the index through st.s. Both st.s (the
// issuing mongos) and shard1 (the coordinator host) still believe shard0 owns the chunk, so
// dropIndexes targets shard0 with a stale shard version, receives a StaleConfig, refreshes, and
// retargets shard2 -- in both the versioned and the DDL-coordinator paths.
ShardVersioningUtil.runOperationOnStaleRouterAfterMoveChunk({
    migrateRouter: st.s1,
    staleRouter: st.s,
    ns,
    toShard: st.shard2,
    bounds: [{x: MinKey}, {x: MaxKey}],
    runStaleOperation: (router) =>
        assert.commandWorked(router.getCollection(ns).dropIndexes({y: 1})),
});

st.stop();
