/**
 * Tests that the dbStats command properly computes the stats by comparing the results from a
 * sharded cluster to the summation of querying the mongod's directly.
 *
 * @tags: [requires_dbstats]
 */

(function() {
"use strict";

// Set up cluster with 2 shards, insert a batch of documents, and configure the cluster so both
// shards have documents.
const st = new ShardingTest({shards: 2, mongos: 1});
const dbName = "db";
const db = st.getDB(dbName);
const collName = "foo";
const ns = dbName + "." + collName;
const numDocs = 100;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

let primaryShard = st.getPrimaryShard(dbName);
let secondaryShard = st.getOther(primaryShard);

let bulk = primaryShard.getCollection(ns).initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({_id: i, x: i, y: -i});
}
assert.commandWorked(bulk.execute());
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: numDocs / 2}}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, find: {_id: 0}, to: secondaryShard.name, _waitForDelete: true}));

const scale = 1024 * 1024;
let dbStats = db.runCommand({dbStats: 1, scale: scale});
assert.commandWorked(dbStats);
jsTestLog('dbStats result on mongos: ' + tojson(dbStats));
let shard0Stats = primaryShard.getDB(dbName).runCommand({dbStats: 1, scale: scale});
assert.commandWorked(shard0Stats);
jsTestLog('dbStats result on primary shard ' + primaryShard.host + ': ' + tojson(shard0Stats));
let shard1Stats = secondaryShard.getDB(dbName).runCommand({dbStats: 1, scale: scale});
assert.commandWorked(shard1Stats);
jsTestLog('dbStats result on secondary shard ' + secondaryShard.host + ': ' + tojson(shard1Stats));

// Compare each of the relevant fields in dbStats to make sure the individual shards' responses sum
// to the overall cluster's value.
let total = shard0Stats.collections + shard1Stats.collections;
assert.eq(dbStats.collections,
          total,
          "Sharded collection dbStats returned " + dbStats.collections +
              " collections total, but sum of individual shards' responses returned " + total +
              " collections total");

total = shard0Stats.views + shard1Stats.views;
assert.eq(dbStats.views,
          total,
          "Sharded collection dbStats returned " + dbStats.views +
              " views total, but sum of individual shards' responses returned " + total +
              " views total");

total = shard0Stats.objects + shard1Stats.objects;
assert.eq(dbStats.objects,
          total,
          "Sharded collection dbStats returned " + dbStats.objects +
              " objects total, but sum of individual shards' responses returned " + total +
              " objects total");

total = shard0Stats.dataSize + shard1Stats.dataSize;
assert.eq(dbStats.dataSize,
          total,
          "Sharded collection dbStats returned " + dbStats.dataSize +
              " dataSize total, but sum of individual shards' responses returned " + total +
              " dataSize total");

total = shard0Stats.storageSize + shard1Stats.storageSize;
assert.eq(dbStats.storageSize,
          total,
          "Sharded collection dbStats returned " + dbStats.storageSize +
              " storageSize total, but sum of individual shards' responses returned " + total +
              " storageSize total");

total = shard0Stats.indexes + shard1Stats.indexes;
assert.eq(dbStats.indexes,
          total,
          "Sharded collection dbStats returned " + dbStats.indexes +
              " indexes total, but sum of individual shards' responses returned " + total +
              " indexes total");

total = shard0Stats.indexSize + shard1Stats.indexSize;
assert.eq(dbStats.indexSize,
          total,
          "Sharded collection dbStats returned " + dbStats.indexSize +
              " indexSize total, but sum of individual shards' responses returned " + total +
              " indexSize total");

total = shard0Stats.totalSize + shard1Stats.totalSize;
assert.eq(dbStats.totalSize,
          total,
          "Sharded collection dbStats returned " + dbStats.totalSize +
              " totalSize total, but sum of individual shards' responses returned " + total +
              " totalSize total");

st.stop();
})();
