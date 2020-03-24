/*
 * Tests the aggregation that collects index consistency metrics for serverStatus retries on stale
 * version errors.
 * @tags: [requires_sharding]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/sharded_index_consistency_metrics_helpers.js");
load("jstests/sharding/libs/shard_versioning_util.js");

// This test creates inconsistent indexes.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

const intervalMS = 10000;
const st = new ShardingTest({
    shards: 2,
    config: 1,
    configOptions: {setParameter: {"shardedIndexConsistencyCheckIntervalMS": intervalMS}}
});

const dbName = "testDb";
const ns0 = dbName + ".testColl0";
const ns1 = dbName + ".testColl1";
const ns2 = dbName + ".testColl2";

// Create 3 sharded collections, two hashed and another with 3 chunks, 1 on shard1 and 2 on shard0.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

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
// for ns2 without refreshing the recipient so at least one shard should typically be stale when the
// periodic index check runs. The check should retry on stale config errors and be able to
// eventually return the correct counter.
assert.commandWorked(st.shard0.getCollection(ns1).createIndex({x: 1}));
assert.soon(
    () => {
        ShardVersioningUtil.moveChunkNotRefreshRecipient(st.s, ns2, st.shard0, st.shard1, {_id: 0});
        sleep(2000);
        ShardVersioningUtil.moveChunkNotRefreshRecipient(st.s, ns2, st.shard1, st.shard0, {_id: 0});
        sleep(2000);

        const latestCount =
            getServerStatusNumCollsWithInconsistentIndexes(st.configRS.getPrimary());
        jsTestLog(
            "Waiting for periodic index check to discover inconsistent indexes. Latest count: " +
            latestCount);
        return latestCount == 2;
    },
    "periodic index check couldn't discover inconsistent indexes with stale shards",
    undefined,
    undefined,
    {runHangAnalyzer: false});

// As an extra sanity check, verify the index check didn't log a failure.
checkLog.containsWithCount(st.configRS.getPrimary(), "Failed to check index consistency", 0);

st.stop();
}());
