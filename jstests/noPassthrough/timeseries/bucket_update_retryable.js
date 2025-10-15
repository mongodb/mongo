/**
 * Bucket Update: exploits field size <32B estimated at 0B.
 * An ordered write that results in a failed bucket update due to exceeding the BSON size limit,
 * should be successfully retryable as a bucket insert.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

const rst = new ReplSetTest({nodes: 1});
const nodes = rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const db = primary.getDB("test");

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.coll;
const bucketsColl = testDB.system.buckets[coll.getName()];
const timeField = "t";
const metaField = "m";

function runTest(isOrderedWrite) {
    jsTestLog("runTest(ordered=" + isOrderedWrite.toString() + ")");

    // Setup

    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));

    const timestamp = ISODate("2025-01-01T12:00:00Z");
    // This value size is chosen to maximize the size of an object that is estimated by the
    // time-series write path of being treated as 0. There ia a fixed-size 8 bytes of bson and
    // estimation overhead.
    const str = "a".repeat(24);
    const measurement1 = {a: 1, [timeField]: timestamp};
    const measurement2 = {};

    // The number of fields chosen below combined with the unestimated size yields a measurement
    // size of around 5MB. When tripled to account for the 3 literals per time-series bucket, this
    // is just underneath the BSON limit for a bucket document.
    for (let i = 0; i < 140000; ++i) {
        measurement2[i.toString()] = str;
    }
    measurement2[timeField] = timestamp;

    // Insert Measurements

    assert.commandWorked(coll.insert(measurement1, {ordered: isOrderedWrite}));

    let stats = coll.stats().timeseries;
    assert.eq(1, stats.numBucketInserts, tojson(stats));
    assert.eq(0, stats.numBucketUpdates, tojson(stats));
    assert.eq(1, stats.numBucketsOpenedDueToMetadata, tojson(stats));
    assert.eq(0, stats.numBucketsClosedDueToSize, tojson(stats));
    assert.eq(0, stats.numBucketDocumentsTooLargeInsert, tojson(stats));
    assert.eq(0, stats.numBucketDocumentsTooLargeUpdate, tojson(stats));

    if (isOrderedWrite) {
        assert.commandWorked(coll.insert(measurement2, {ordered: isOrderedWrite}));
    } else {
        assert.commandFailedWithCode(coll.insert(measurement2, {ordered: isOrderedWrite}),
                                     ErrorCodes.BSONObjectTooLarge);
    }

    stats = coll.stats().timeseries;
    assert.eq(isOrderedWrite ? 2 : 1, stats.numBucketInserts, tojson(stats));
    assert.eq(0, stats.numBucketUpdates, tojson(stats));
    // The first bucket gets cleared during the retry logic of an ordered write, thus when the
    // second bucket gets allocated, the write path doesn't see an associated open bucket for the
    // same meta value.
    assert.eq(isOrderedWrite ? 2 : 1, stats.numBucketsOpenedDueToMetadata, tojson(stats));
    assert.eq(0, stats.numBucketsClosedDueToSize, tojson(stats));
    assert.eq(0, stats.numBucketDocumentsTooLargeInsert, tojson(stats));
    assert.eq(1, stats.numBucketDocumentsTooLargeUpdate, tojson(stats));

    // Check Results
    // TODO(SERVER-108699): Remove this check.

    let buckets = bucketsColl.find().toArray();
    for (let i = 0; i < buckets.length; i++) {
        let bucketDocSize = Object.bsonsize(buckets[i]);
        assert.lte(bucketDocSize, 16 * 1024 * 1024);
    }

    coll.drop();
}

runTest(/*isOrderedWrite=*/ true);
runTest(/*isOrderedWrite=*/ false);

rst.stopSet();
