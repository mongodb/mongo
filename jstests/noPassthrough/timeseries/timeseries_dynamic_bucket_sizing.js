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
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const minWiredTigerCacheSizeGB = 0.256;
const cacheSize = minWiredTigerCacheSizeGB * 1000 * 1000 * 1000;  // 256 MB
const defaultBucketMaxSize = 128000;                              // 125 KB
const minBucketCount = 10;
const timeFieldName = 'time';
const metaFieldName = 'meta';
const timestamp = ISODate('2023-02-13T01:00:00Z');
const collName = 't';

// A cardinality higher than this calculated value will call for smaller bucket size limit caused
// by cache pressure.
const cardinalityForCachePressure = Math.ceil(cacheSize / (2 * defaultBucketMaxSize));  // 1000

const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {wiredTigerCacheSizeGB: minWiredTigerCacheSizeGB},
});
replSet.startSet({
    setParameter:
        {timeseriesBucketMaxSize: defaultBucketMaxSize, timeseriesLargeMeasurementThreshold: 1}
});
replSet.initiate();

const db = replSet.getPrimary().getDB(jsTestName());

const alwaysUseCompressedBuckets = TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db);

let coll = db.getCollection(collName + '1');
coll.drop();
assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

// Helper to log timeseries stats.
const formatStatsLog = ((stats) => {
    return "Timeseries stats: " + tojson(stats);
});

// Inserts documents into the collection with increasing meta fields to generate N buckets. We make
// sure to exceed the bucket min count per bucket to bypass large measurement checks. After this
// call we should have numOfBuckets buckets each with size of around ~12KB.
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

// We insert enough data to cause buckets to roll over due to their size exceeding the maximum
// bucket size. Because the cardinality is below the threshold at which the maximum bucket size
// derived from cache pressure is smaller than the default maximum size, no buckets should be
// closed due to cache pressure. Before this insertion each bucket should have a size of about
// 12KB, so attempting an insertion of 120KB puts it at around 132KB, which is large enough to
// roll over due to the default maximum bucket size (128KB) but not the cache derived maximum
// bucket size (137KB).
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < belowCardinalityThreshold; i++) {
    bulk.insert(
        {_id: '00' + i, [timeFieldName]: timestamp, [metaFieldName]: i, value: "a".repeat(120000)});
}

assert.commandWorked(bulk.execute());
timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
bucketsClosedDueToSize = timeseriesStats.numBucketsClosedDueToSize;
bucketsClosedDueToCachePressure = timeseriesStats.numBucketsClosedDueToCachePressure;
compressedBuckets = timeseriesStats.numCompressedBuckets;

// We should be closing buckets due to the default size constraints. No buckets should be closed
// due to cache pressure.
assert.eq(bucketsClosedDueToSize, cardinalityForCachePressure, formatStatsLog(timeseriesStats));
assert.eq(bucketsClosedDueToCachePressure, 0, formatStatsLog(timeseriesStats));
if (!alwaysUseCompressedBuckets) {
    assert.eq(compressedBuckets, cardinalityForCachePressure, formatStatsLog(timeseriesStats));
}

// Create a new collection to test closing buckets due to cache pressure. Since the cardinality of
// buckets is now high enough to make the cache derived maximum bucket size smaller than the
// default maximum bucket size, buckets should begin to close because of cache pressure rather than
// size.
coll = db.getCollection(collName + '2');
coll.drop();
assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

// If we pass the cardinality point to simulate cache pressure, we will begin to see buckets
// closed due to 'CachePressure' and not 'DueToSize'.
const aboveCardinalityThreshold = cardinalityForCachePressure * 3 / 2;
initializeBucketsPastMinCount(aboveCardinalityThreshold);

timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
bucketsClosedDueToSize = timeseriesStats.numBucketsClosedDueToSize;
bucketsClosedDueToCachePressure = timeseriesStats.numBucketsClosedDueToCachePressure;
compressedBuckets = timeseriesStats.numCompressedBuckets;

// Ensure we have not closed any buckets due to size or cache pressure.
assert.eq(bucketsClosedDueToSize, 0, formatStatsLog(timeseriesStats));
assert.eq(bucketsClosedDueToCachePressure, 0, formatStatsLog(timeseriesStats));

// We insert 80KB of data into the buckets. After initialization the buckets should have a size of
// about 10KB, and after adding 80KB of data they should be at around 90KB. This should be greater
// than the cache derived maximum bucket size, should be about 55KB, but significantly below the
// default maximum bucket size, which is still 128KB. Therefore, we should see buckets closing
// due to cache pressure but none due to size.
bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < aboveCardinalityThreshold; i++) {
    bulk.insert(
        {_id: '00' + i, [timeFieldName]: timestamp, [metaFieldName]: i, value: "a".repeat(80000)});
}
assert.commandWorked(bulk.execute());

timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
bucketsClosedDueToSize = timeseriesStats.numBucketsClosedDueToSize;
bucketsClosedDueToCachePressure = timeseriesStats.numBucketsClosedDueToCachePressure;
compressedBuckets = timeseriesStats.numCompressedBuckets;

// We expect 'bucketsClosedDueToSize' to be 0.
assert.eq(bucketsClosedDueToSize, 0, formatStatsLog(timeseriesStats));

// Previously, the bucket max size was 128KB, but under cache pressure using
// 'aboveCardinalityThreshold', the max size drops to roughly ~55KB. Therfore, all of the buckets
// should have been closed due to cache pressure.
assert.eq(
    bucketsClosedDueToCachePressure, aboveCardinalityThreshold, formatStatsLog(timeseriesStats));

if (!alwaysUseCompressedBuckets) {
    assert.eq(compressedBuckets, aboveCardinalityThreshold, formatStatsLog(timeseriesStats));
}

replSet.stopSet();
