/**
 * Evaluate the behaviour of bucket closure when we simulate high cache pressure due to a high
 * cardinality workload. After we hit a certain cardinality (the number of active buckets generated
 * in this test by distinct metaField values) we expect buckets to be closed with a smaller bucket
 * size limit to alleviate pressure on the cache with respect to large measurement insertions.
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

const defaultBucketMaxSize = 128000;                                           //  125 KB
const minWiredTigerCacheSizeGB = 0.256;                                        //  256 MB
const minWiredTigerCacheSize = minWiredTigerCacheSizeGB * 1024 * 1024 * 1024;  //  256 MB
const measurementValueLength = 1 * 1024 * 1024;                                //    1 MB
const defaultBucketMinCount = 10;

const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {wiredTigerCacheSizeGB: minWiredTigerCacheSizeGB},
});
replSet.startSet({setParameter: {timeseriesBucketMaxSize: defaultBucketMaxSize}});
replSet.initiate();

const db = replSet.getPrimary().getDB("test");
let coll = db.getCollection('t');
coll.drop();

if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) {
    replSet.stopSet();
    jsTestLog(
        'Skipping test because the TimeseriesScalabilityImprovements feature flag is disabled.');
    return;
}

const timeFieldName = 'time';
const metaFieldName = 'meta';

const resetCollection = (() => {
    coll.drop();
    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
});

// Inserts small documents into the collection with increasing meta fields to generate N buckets.
const initializeBuckets = function(numOfBuckets = 1) {
    jsTestLog("Inserting and generating buckets.");
    let batch = [];
    for (let i = 0; i < numOfBuckets; i++) {
        const doc = {_id: i, [timeFieldName]: ISODate(), [metaFieldName]: i, value: "a"};
        batch.push(doc);
    }
    assert.commandWorked(coll.insertMany(batch));
};

(function largeMeasurementsNoCachePressure() {
    jsTestLog("Entering largeMeasurementsNoCachePressure...");
    coll = db.getCollection('largeMeasurementsNoCachePressure');
    resetCollection();

    let expectedBucketCount = 0;
    let numBucketsClosedDueToSize = 0;
    let numBucketsClosedDueToCachePressure = 0;
    let numCompressedBuckets = 0;

    const meta1 = 1;
    const meta2 = 2;

    // Insert 9 large measurements into same bucket (mapping to meta1) resulting in a bucket of size
    // ~11.5 MB (right under the largest size of buckets we allow which is 12 MB).
    for (let i = 0; i < defaultBucketMinCount - 1; i++) {
        const doc = {
            _id: i,
            [timeFieldName]: ISODate(),
            [metaFieldName]: meta1,
            value: "a".repeat(measurementValueLength)
        };
        assert.commandWorked(coll.insert(doc));
    }
    expectedBucketCount++;

    let timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
    assert.eq(timeseriesStats.bucketCount, expectedBucketCount);
    assert.eq(timeseriesStats.numBucketsClosedDueToSize, numBucketsClosedDueToSize);
    assert.eq(timeseriesStats.numBucketsClosedDueToCachePressure,
              numBucketsClosedDueToCachePressure);
    assert.eq(timeseriesStats.numCompressedBuckets, numCompressedBuckets);

    // If we exceed the min bucket count of 10, we should close the bucket since it exceeds our
    // default bucket size of 125 KB. (This requires two additional insertions).
    const doc = {_id: 4, [timeFieldName]: ISODate(), [metaFieldName]: meta1, value: "a"};
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.insert(doc));

    expectedBucketCount++;
    numBucketsClosedDueToSize++;
    numCompressedBuckets++;

    timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
    assert.eq(timeseriesStats.bucketCount, expectedBucketCount);
    assert.eq(timeseriesStats.numBucketsClosedDueToSize, numBucketsClosedDueToSize);
    assert.eq(timeseriesStats.numBucketsClosedDueToCachePressure,
              numBucketsClosedDueToCachePressure);
    assert.eq(timeseriesStats.numCompressedBuckets, numCompressedBuckets);

    // Since the maximum size for buckets is capped at 12 MB, we should hit the size limit before
    // closing the bucket due to the minimum count, so we expect to close the oversized bucket and
    // create another bucket.
    for (let i = 0; i < defaultBucketMinCount; i++) {
        const doc = {
            _id: i,
            [timeFieldName]: ISODate(),
            [metaFieldName]: meta2,
            value: "b".repeat(measurementValueLength)
        };
        assert.commandWorked(coll.insert(doc));
    }

    // We create one bucket for 'meta2', fill it up and create another one for future insertions.
    expectedBucketCount += 2;
    numBucketsClosedDueToSize++;
    numCompressedBuckets++;

    timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
    assert.eq(timeseriesStats.bucketCount, expectedBucketCount);
    assert.eq(timeseriesStats.numBucketsClosedDueToSize, numBucketsClosedDueToSize);
    assert.eq(timeseriesStats.numBucketsClosedDueToCachePressure,
              numBucketsClosedDueToCachePressure);
    assert.eq(timeseriesStats.numCompressedBuckets, numCompressedBuckets);
})();

(function largeMeasurementsWithCachePressure() {
    jsTestLog("Entering largeMeasurementsWithCachePressure...");
    coll = db.getCollection('largeMeasurementsWithCachePressure');
    resetCollection();

    // We want the 'cacheDerivedMaxSize' to equal 5.5 MB.
    const cacheDerivedMaxSize = 5.5 * 1024 * 1024;
    const bucketCount =
        Math.ceil(minWiredTigerCacheSize / (2 * cacheDerivedMaxSize));  // Evaluates to 24.
    const meta = bucketCount;

    // We expect the bucket mapping to 'meta' to be around ~5 MB in size so no buckets should be
    // closed yet. We generate a cardinality equal to 'bucketCount'.
    initializeBuckets(bucketCount - 1);
    for (let i = 0; i < 3; i++) {
        const doc = {
            _id: i,
            [timeFieldName]: ISODate(),
            [metaFieldName]: meta,
            value: "a".repeat(measurementValueLength)
        };
        assert.commandWorked(coll.insert(doc));
    }

    let timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
    assert.eq(timeseriesStats.bucketCount, bucketCount);
    assert.eq(timeseriesStats.numBucketsClosedDueToSize, 0);
    assert.eq(timeseriesStats.numBucketsClosedDueToCachePressure, 0);
    assert.eq(timeseriesStats.numCompressedBuckets, 0);

    // We expect this insert to cause the bucket to close due to cache pressure since it will exceed
    // the rough cacheDerivedMaxSize of 5.5 MB and create a new bucket for this measurement.
    const doc = {
        _id: bucketCount,
        [timeFieldName]: ISODate(),
        [metaFieldName]: meta,
        value: "a".repeat(measurementValueLength)
    };
    assert.commandWorked(coll.insert(doc));

    timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
    assert.eq(timeseriesStats.bucketCount, bucketCount + 1);
    assert.eq(timeseriesStats.numBucketsClosedDueToSize, 0);
    assert.eq(timeseriesStats.numBucketsClosedDueToCachePressure, 1);
    assert.eq(timeseriesStats.numCompressedBuckets, 1);
})();

replSet.stopSet();
})();
