/**
 * Evaluate the behaviour of bucket closure when we simulate high cache pressure due to a high
 * cardinality workload. After we hit a certain cardinality (the number of active buckets generated
 * in this test by distinct metaField values) we expect buckets to be closed with a smaller bucket
 * size limit to alleviate pressure on the cache.
 *
 * @tags: [
 *   # Exclude in-memory engine, rollbacks due to pinned cache content rely on eviction.
 *   requires_persistence,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const minWiredTigerCacheSizeGB = 0.256;
const cacheSize = minWiredTigerCacheSizeGB * 1000 * 1000 * 1000;  // 256 MB
const defaultBucketMaxSize = 128000;                              // 125 KB
const minBucketCount = 10;
const timeFieldName = 'time';
const metaFieldName = 'meta';
const timestamp = ISODate('2023-02-13T01:00:00Z');

// A cardinality higher than this calculated value will call for smaller bucket size limit caused by
// cache pressure.
const cardinalityForCachePressure = Math.ceil(cacheSize / (2 * defaultBucketMaxSize));  // 1000

const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {wiredTigerCacheSizeGB: minWiredTigerCacheSizeGB},
});
replSet.startSet({setParameter: {timeseriesBucketMaxSize: defaultBucketMaxSize}});
replSet.initiate();

const db = replSet.getPrimary().getDB(jsTestName());
const coll = db.getCollection('t');
coll.drop();
assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) {
    replSet.stopSet();
    jsTestLog(
        'Skipping test because the TimeseriesScalabilityImprovements feature flag is disabled.');
    return;
}

// Helper to log timeseries stats.
const formatStatsLog = ((stats) => {
    return "Timeseries stats: " + tojson(stats);
});

// Inserts documents into the collection with increasing meta fields to generate N buckets. We make
// sure to exceed the bucket min count per bucket to bypass large measurement checks.
const initializeBucketsPastMinCount = function(numOfBuckets = 1) {
    jsTestLog("Inserting and generating buckets. Targeting '" + numOfBuckets + "' buckets.");
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numOfBuckets; i++) {
        for (let j = 0; j < minBucketCount; ++j) {
            const doc = {
                _id: '' + i + j,
                [timeFieldName]: timestamp,
                [metaFieldName]: i,
                value: "a".repeat(1000)
            };
            bulk.insert(doc);
        }
    }
    assert.commandWorked(bulk.execute());
};

const belowCardinalityThreshold = cardinalityForCachePressure;
initializeBucketsPastMinCount(belowCardinalityThreshold);

let timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
let bucketsClosedDueToSize = timeseriesStats.numBucketsClosedDueToSize;
let bucketsClosedDueToCachePressure = timeseriesStats.numBucketsClosedDueToCachePressure;
let compressedBuckets = timeseriesStats.numCompressedBuckets;

// Ensure we have not closed any buckets due to size or cache pressure.
assert.eq(bucketsClosedDueToSize, 0, formatStatsLog(timeseriesStats));
assert.eq(bucketsClosedDueToCachePressure, 0, formatStatsLog(timeseriesStats));
assert.eq(timeseriesStats.bucketCount, belowCardinalityThreshold, formatStatsLog(timeseriesStats));

// We only end up doing two passes before we start to close buckets due to size limits.
while (bucketsClosedDueToSize == 0) {
    jsTestLog("Inserting 50000 bytes of data into buckets.");
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < belowCardinalityThreshold; i++) {
        bulk.insert({
            _id: '00' + i,
            [timeFieldName]: timestamp,
            [metaFieldName]: i,
            value: "a".repeat(30000)
        });
        bulk.insert({
            _id: '00' + i,
            [timeFieldName]: timestamp,
            [metaFieldName]: i,
            value: "a".repeat(20000)
        });
    }
    assert.commandWorked(bulk.execute());

    timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
    bucketsClosedDueToSize = timeseriesStats.numBucketsClosedDueToSize;
    bucketsClosedDueToCachePressure = timeseriesStats.numBucketsClosedDueToCachePressure;
    compressedBuckets = timeseriesStats.numCompressedBuckets;
}

// On the second pass of inserts, we will close buckets due to the default size constraints. No
// buckets should be closed due to cache pressure.
assert.eq(bucketsClosedDueToSize, cardinalityForCachePressure, formatStatsLog(timeseriesStats));
assert.eq(bucketsClosedDueToCachePressure, 0, formatStatsLog(timeseriesStats));
assert.eq(compressedBuckets, cardinalityForCachePressure, formatStatsLog(timeseriesStats));

// If we pass the cardinality point to simulate cache pressure, we will begin to see buckets closed
// due to 'CachePressure' and not 'DueToSize'.
const aboveCardinalityThreshold = cardinalityForCachePressure * 3 / 2;
initializeBucketsPastMinCount(aboveCardinalityThreshold);

let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < aboveCardinalityThreshold; i++) {
    bulk.insert(
        {_id: '00' + i, [timeFieldName]: timestamp, [metaFieldName]: i, value: "a".repeat(20000)});
}
assert.commandWorked(bulk.execute());

timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
bucketsClosedDueToSize = timeseriesStats.numBucketsClosedDueToSize;
bucketsClosedDueToCachePressure = timeseriesStats.numBucketsClosedDueToCachePressure;
compressedBuckets = timeseriesStats.numCompressedBuckets;

// We expect 'bucketsClosedDueToSize' to remain the same but 'bucketsClosedDueToCachePressure' to
// increase.
assert.eq(bucketsClosedDueToSize, cardinalityForCachePressure, formatStatsLog(timeseriesStats));

// Previously, the bucket max size was 128000 bytes, but under cache pressure using
// 'aboveCardinalityThreshold', the max size drops to roughly ~85334. This means the old
// measurements (up to 'cardinalityForCachePressure') will need to be closed since they are sized at
// ~120000 bytes. The newly inserted measurements are only sized at ~(20000 * 3) bytes so stay open.
assert.eq(
    bucketsClosedDueToCachePressure, cardinalityForCachePressure, formatStatsLog(timeseriesStats));

// We expect the number of compressed buckets to double (independent to whether the buckets were
// closed due to size or cache pressure).
assert.eq(compressedBuckets, 2 * cardinalityForCachePressure, formatStatsLog(timeseriesStats));

replSet.stopSet();
})();
