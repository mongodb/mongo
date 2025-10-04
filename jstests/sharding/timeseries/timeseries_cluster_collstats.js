/**
 * Tests that the cluster collStats command returns timeseries statistics in the expected format.
 *
 * For legacy collStats command:
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
 * For aggregate $collStats stage:
 * [
 *  {
 *     ....,
 *     "ns" : ...,
 *     "shard" : ...,
 *     "latencyStats" : {
 *         ....
 *     },
 *     "storageStats" : {
 *         ...,
 *        "timeseries" : {
 *         ...,
 *         },
 *     },
 *     "count" : {
 *         ....
 *     },
 *     "queryExecStats" : {
 *         ....
 *     },
 *  },
 *  {
 *   .... (Other shard's result)
 *  },
 *  ...
 * ]
 *
 * @tags: [
 *   # TODO (SERVER-98385): Remove this tag.
 *   multiversion_incompatible,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {
    areViewlessTimeseriesEnabled,
    getTimeseriesCollForDDLOps,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const numShards = 2;
const st = new ShardingTest({shards: numShards});

const dbName = "testDB";
const collName = "testColl";
const mongosDB = st.s.getDB(dbName);
const mongosColl = mongosDB.getCollection(collName);
const timeField = "tm";
const metaField = "mt";
const emptyCollName = "testEmptyColl";

// Create a timeseries collection.
assert.commandWorked(mongosDB.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));
assert.commandWorked(
    mongosDB.createCollection(emptyCollName, {timeseries: {timeField: timeField, metaField: metaField}}),
);

// Populate the timeseries collection with some data. More interesting test case, and populates the
// statistics results.
const numberDoc = 20;
for (let i = 0; i < numberDoc; i++) {
    assert.commandWorked(mongosColl.insert({[timeField]: ISODate(), [metaField]: i}));
}
assert.eq(mongosColl.find().itcount(), numberDoc);

// The cluster collStats command should pull the shard's 'timeseries' data to the top level of the
// command results.
let clusterCollStatsResult = assert.commandWorked(mongosDB.runCommand({collStats: collName}));
jsTestLog("Cluster collStats command result: " + tojson(clusterCollStatsResult));
assert(
    clusterCollStatsResult.timeseries,
    "Expected a top-level 'timeseries' field but didn't find one: " + tojson(clusterCollStatsResult),
);

// Check that the top-level 'timeseries' fields match the primary shard's, that the stats were
// correctly pulled up.
const primaryShard = st.getPrimaryShard(dbName);
const otherShard = st.getOther(primaryShard);
assert(
    clusterCollStatsResult.shards[primaryShard.shardName].timeseries,
    "Expected a shard 'timeseries' field but didn't find one: " + tojson(clusterCollStatsResult),
);
assert.docEq(clusterCollStatsResult.timeseries, clusterCollStatsResult.shards[primaryShard.shardName].timeseries);

// Shard the timeseries collection
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(mongosColl.createIndex({[metaField]: 1}));
assert.commandWorked(
    st.s.adminCommand({
        shardCollection: mongosColl.getFullName(),
        key: {[metaField]: 1},
    }),
);

// Check that timeseries collStats are all 0 on an empty sharded collection
clusterCollStatsResult = assert.commandWorked(mongosDB.runCommand({collStats: emptyCollName}));
jsTestLog("Empty cluster collStats command result: " + tojson(clusterCollStatsResult));
assert.eq(0, TimeseriesTest.getStat(clusterCollStatsResult.timeseries, "avgBucketSize"));
assert.eq(0, TimeseriesTest.getStat(clusterCollStatsResult.timeseries, "avgNumMeasurementsPerCommit"));
assert.eq(0, TimeseriesTest.getStat(clusterCollStatsResult.timeseries, "bucketCount"));
assert.eq(0, TimeseriesTest.getStat(clusterCollStatsResult.timeseries, "numBucketFetchesFailed"));
assert.eq(0, TimeseriesTest.getStat(clusterCollStatsResult.timeseries, "numBucketInserts"));
assert.eq(0, TimeseriesTest.getStat(clusterCollStatsResult.timeseries, "numBucketQueriesFailed"));
assert.eq(0, TimeseriesTest.getStat(clusterCollStatsResult.timeseries, "numBucketUpdates"));
TimeseriesTest.checkBucketReopeningsFailedCounters(clusterCollStatsResult.timeseries, {});

// Force splitting numShards chunks.
const splitPoint = {
    meta: numberDoc / numShards,
};
assert.commandWorked(
    st.s.adminCommand({split: getTimeseriesCollForDDLOps(mongosDB, mongosColl).getFullName(), middle: splitPoint}),
);
// Ensure that currently both chunks reside on the primary shard.
let counts = st.chunkCounts(collName, dbName);
assert.eq(numShards, counts[primaryShard.shardName]);
// Move one of the chunks into the second shard.
assert.commandWorked(
    st.s.adminCommand({
        movechunk: getTimeseriesCollForDDLOps(mongosDB, mongosColl).getFullName(),
        find: splitPoint,
        to: otherShard.name,
        _waitForDelete: true,
    }),
);
// Ensure that each shard owns one chunk.
counts = st.chunkCounts(collName, dbName);
assert.eq(1, counts[primaryShard.shardName], counts);
assert.eq(1, counts[otherShard.shardName], counts);

// Do some insertion after moving chunks.
for (let i = 0; i < numberDoc; i++) {
    assert.commandWorked(mongosColl.insert({[timeField]: ISODate(), [metaField]: i}));
}
assert.eq(mongosColl.find().itcount(), numberDoc * 2);

function checkAllFieldsAreInResult(result) {
    assert(result.hasOwnProperty("latencyStats"), result);
    assert(result.hasOwnProperty("storageStats"), result);
    assert(result.hasOwnProperty("count"), result);
    assert(result.hasOwnProperty("queryExecStats"), result);
}

function assertTimeseriesAggregationCorrectness(total, shards) {
    // TODO SERVER-106617: review if checking for the bucketsNs should be re-enabled for viewless
    // timeseries.
    if (!areViewlessTimeseriesEnabled(mongosDB)) {
        assert(shards.every((x) => x.bucketsNs === total.bucketsNs));
    }
    assert.eq(
        total.bucketCount,
        shards.map((x) => x.bucketCount).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert.eq(
        total.avgBucketSize,
        Math.floor(
            shards.map((x) => x.avgBucketSize * x.bucketCount).reduce((x, y) => x + y, 0 /* initial value */) /
                total.bucketCount,
        ),
    );
    assert.eq(
        total.avgNumMeasurementsPerCommit,
        shards.map((x) => x.numMeasurementsCommitted).reduce((x, y) => x + y, 0 /* initial value */) /
            shards.map((x) => x.numCommits).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert.eq(
        total.numBucketInserts,
        shards.map((x) => x.numBucketInserts).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert.eq(
        total.numBucketUpdates,
        shards.map((x) => x.numBucketUpdates).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert.eq(
        total.numBucketsOpenedDueToMetadata,
        shards.map((x) => x.numBucketsOpenedDueToMetadata).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert.eq(
        total.numBucketsClosedDueToCount,
        shards.map((x) => x.numBucketsClosedDueToCount).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert.eq(
        total.numBucketsClosedDueToSize,
        shards.map((x) => x.numBucketsClosedDueToSize).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert.eq(
        total.numBucketsClosedDueToTimeForward,
        shards.map((x) => x.numBucketsClosedDueToTimeForward).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert.eq(
        total.numBucketsClosedDueToMemoryThreshold,
        shards.map((x) => x.numBucketsClosedDueToMemoryThreshold).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert.eq(
        total.numCommits,
        shards.map((x) => x.numCommits).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert.eq(
        total.numWaits,
        shards.map((x) => x.numWaits).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert.eq(
        total.numMeasurementsCommitted,
        shards.map((x) => x.numMeasurementsCommitted).reduce((x, y) => x + y, 0 /* initial value */),
    );
    assert(total.bucketCount > 0);
    assert(total.avgBucketSize > 0);
    assert(total.avgNumMeasurementsPerCommit > 0);
    assert(total.numBucketInserts > 0);
    assert(total.numCommits > 0);
    assert(total.numMeasurementsCommitted > 0);
}

function verifyClusterCollStatsResult(clusterCollStatsResult, sumTimeseriesStatsAcrossShards, isAggregation) {
    if (isAggregation) {
        // $collStats should output one document per shard.
        assert.eq(
            clusterCollStatsResult.length,
            numShards,
            "Expected " + numShards + "documents to be returned: " + tojson(clusterCollStatsResult),
        );

        checkAllFieldsAreInResult(clusterCollStatsResult[0]);
        checkAllFieldsAreInResult(clusterCollStatsResult[1]);
    }

    assert(
        sumTimeseriesStatsAcrossShards,
        "Expected an aggregated 'timeseries' field but didn't find one: " + tojson(clusterCollStatsResult),
    );

    const primaryShardStats = isAggregation
        ? clusterCollStatsResult[0].storageStats.timeseries
        : clusterCollStatsResult.shards[primaryShard.shardName].timeseries;

    const otherShardStats = isAggregation
        ? clusterCollStatsResult[1].storageStats.timeseries
        : clusterCollStatsResult.shards[otherShard.shardName].timeseries;

    // Check that the top-level 'timeseries' fields match the sum of two shard's, that the stats
    // were correctly aggregated.
    assert(
        primaryShardStats,
        "Expected a shard 'timeseries' field on shard " +
            primaryShard.shardName +
            " but didn't find one: " +
            tojson(clusterCollStatsResult),
    );
    assert(
        otherShardStats,
        "Expected a shard 'timeseries' field on shard " +
            otherShard.shardName +
            " but didn't find one: " +
            tojson(clusterCollStatsResult),
    );

    assertTimeseriesAggregationCorrectness(sumTimeseriesStatsAcrossShards, [primaryShardStats, otherShardStats]);
}

//  Tests that the output of the collStats command returns results from both the shards and
//  includes all the expected fields.
clusterCollStatsResult = assert.commandWorked(mongosDB.runCommand({collStats: collName}));
jsTestLog("Sharded cluster collStats command result: " + tojson(clusterCollStatsResult));
const sumTimeseriesStatsAcrossShards = clusterCollStatsResult.timeseries;
verifyClusterCollStatsResult(
    clusterCollStatsResult,
    sumTimeseriesStatsAcrossShards,
    false, // isAggregation
);

// Tests that the output of the $collStats stage returns results from both the shards and includes
// all the expected fields.
clusterCollStatsResult = mongosColl
    .aggregate([{$collStats: {latencyStats: {}, storageStats: {}, count: {}, queryExecStats: {}}}])
    .toArray();
jsTestLog("Sharded cluster collStats aggregation result: " + tojson(clusterCollStatsResult));

// Use the same sumTimeseriesStatsAcrossShards value as the collStats command since
// aggregation does not sum up timeseries stats results. This will also verify that the results
// output by collStats in find and aggregation are the same.
verifyClusterCollStatsResult(
    clusterCollStatsResult,
    sumTimeseriesStatsAcrossShards,
    true, // isAggregation
);

st.stop();
