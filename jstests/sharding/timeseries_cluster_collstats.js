/**
 * Tests that the cluster collStats command returns timeseries statistics in the expected format.
 *
 * {
 *     ....,
 *     "ns" : ...,
 *     ....,
 *     "timeseries" : {
 *         .... (sums the shards' field values)
 *     }
 *     ....,
 *     "shards" : {
 *         <shardName> {
 *             "timeseries" : {
 *                 .... (single shard's field values)
 *             }
 *             ....
 *         }
 *     }
 *     ....
 * }
 *
 * @tags: [
 * ]
 */

(function() {
load("jstests/core/timeseries/libs/timeseries.js");

// Sharded timeseries collections are not yet supported. Therefore, the cluster will not possess the
// same collections/indexes.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

const st = new ShardingTest({shards: 2});

if (!TimeseriesTest.timeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    st.stop();
    return;
}

const dbName = 'testDB';
const mongosDB = st.s.getDB(dbName);
const collName = 'testColl';
const mongosColl = mongosDB.getCollection(collName);

// Create a timeseries collection.
assert.commandWorked(
    mongosDB.createCollection(collName, {timeseries: {timeField: 'tm', metaField: 'xx'}}));

// Populate the timeseries collection with some data. More interesting test case, and populates the
// statistics results.
const numberDoc = 20;
for (let i = 0; i < numberDoc; i++) {
    assert.commandWorked(mongosColl.insert({'tm': ISODate(), 'xx': i}));
}
assert.eq(mongosColl.count(), numberDoc);

// The cluster collStats command should pull the shard's 'timeseries' data to the top level of the
// command results.
const clusterCollStatsResult = assert.commandWorked(mongosDB.runCommand({collStats: collName}));
jsTestLog("Cluster collStats command result: " + tojson(clusterCollStatsResult));
assert(clusterCollStatsResult.timeseries,
       "Expected a top-level 'timeseries' field but didn't find one: " +
           tojson(clusterCollStatsResult));

// Check that the top-level 'timeseries' fields match the shard's, that the stats were correctly
// pulled up.
assert(
    clusterCollStatsResult.shards["timeseries_cluster_collstats-rs1"].timeseries,
    "Expected a shard 'timeseries' field but didn't find one: " + tojson(clusterCollStatsResult));
assert.docEq(clusterCollStatsResult.timeseries,
             clusterCollStatsResult.shards["timeseries_cluster_collstats-rs1"].timeseries);

st.stop();
})();
