/**
 * Tests the aggregation that collects index consistency metrics for serverStatus retries on stale
 * version errors.
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunks.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    checkServerStatusNumCollsWithInconsistentIndexes,
    getServerStatusNumCollsWithInconsistentIndexes,
} from "jstests/noPassthrough/libs/sharded_index_consistency_metrics_helpers.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

// This test creates inconsistent indexes.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

const intervalMS = 10000;
const st = new ShardingTest({
    shards: 2,
    config: 1,
    configOptions: {setParameter: {"shardedIndexConsistencyCheckIntervalMS": intervalMS}},
});

const dbName = "testDb";
const collName2 = "testColl2";
const ns0 = dbName + ".testColl0";
const ns1 = dbName + ".testColl1";
const ns2 = dbName + ".testColl2";

// Create 3 sharded collections, two hashed and another with 3 chunks, 1 on shard1 and 2 on shard0.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

assert.commandWorked(st.s.adminCommand({shardCollection: ns0, key: {_id: "hashed"}}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns1, key: {_id: "hashed"}}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns2, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns2, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns2, middle: {_id: 10}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns2, find: {_id: 10}, to: st.shard1.shardName}));

// Wait for the periodic index check to run by creating an inconsistency and waiting for the new
// counter value to be reported.
assert.commandWorked(st.shard0.getCollection(ns0).createIndex({x: 1}));
checkServerStatusNumCollsWithInconsistentIndexes(st.configRS.getPrimary(), 1);

// Clear the config server's log before the check below so an earlier failure to load inconsistent
// indexes won't trigger a failure when the logs are checked below.
assert.commandWorked(st.configRS.getPrimary().adminCommand({clearLog: "global"}));

// Create an index inconsistency on ns1 and then begin repeatedly moving a chunk between both shards
// for ns2. A shard can never be ignorant of its own metadata under authoritative shards, but the
// config server's catalog cache lags behind the migrations, so its periodic index check sends stale
// shard versions and must retry on stale config errors. Wait until the check both returns the
// correct counter and has observed at least one stale config error (so the retry path is actually
// exercised rather than passing trivially).
assert.commandWorked(st.shard0.getCollection(ns1).createIndex({x: 1}));
// The config server runs the periodic index check as a router, resolving routing from its own
// (lazily-refreshed) catalog cache.
const staleConfigCountStart = ShardVersioningUtil.getRouterStaleConfigErrorCount(
    st.configRS.getPrimary(),
);
assert.soon(
    () => {
        ChunkHelper.moveChunk(
            st.s.getDB(dbName),
            collName2,
            [{_id: 0}, {_id: 10}],
            st.shard1.shardName,
            true /* waitForDelete */,
        );
        sleep(2000);
        ChunkHelper.moveChunk(
            st.s.getDB(dbName),
            collName2,
            [{_id: 0}, {_id: 10}],
            st.shard0.shardName,
            true /* waitForDelete */,
        );
        sleep(2000);

        const latestCount = getServerStatusNumCollsWithInconsistentIndexes(
            st.configRS.getPrimary(),
        );
        const observedStaleConfig =
            ShardVersioningUtil.getRouterStaleConfigErrorCount(st.configRS.getPrimary()) >
            staleConfigCountStart;
        jsTestLog(
            "Waiting for periodic index check to discover inconsistent indexes. Latest count: " +
                latestCount +
                ", observed stale config: " +
                observedStaleConfig,
        );
        return latestCount == 2 && observedStaleConfig;
    },
    "periodic index check couldn't discover inconsistent indexes while the config server was stale",
    undefined,
    undefined,
    {runHangAnalyzer: false},
);

// As an extra sanity check, verify the index check didn't log a failure.
checkLog.containsWithCount(st.configRS.getPrimary(), "Failed to check index consistency", 0);

st.stop();
