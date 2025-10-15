/**
 * Bucket Update: Large meta near the BSON limit, allows only one measurement due to lower
 * timeseries bucket size limits - Bucket::kLargeMeasurementsMaxBucketSize.
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
    const meta = "a".repeat(1024 * 1024 * 15);
    const measurement1 = {a: 1, [timeField]: timestamp, [metaField]: meta};
    const measurement2 = {
        a: 2,
        [timeField]: ISODate("2025-01-01T12:00:00Z"),
        [metaField]: meta,
        b: "b".repeat(1024 * 1024 * 0.335),
    };

    // Insert Measurements

    assert.commandWorked(coll.insert(measurement1, {ordered: isOrderedWrite}));

    let stats = coll.stats().timeseries;
    assert.eq(1, stats.numBucketInserts, tojson(stats));
    assert.eq(0, stats.numBucketUpdates, tojson(stats));
    assert.eq(1, stats.numBucketsOpenedDueToMetadata, tojson(stats));
    assert.eq(0, stats.numBucketsClosedDueToSize, tojson(stats));
    assert.eq(0, stats.numBucketDocumentsTooLargeInsert, tojson(stats));
    assert.eq(0, stats.numBucketDocumentsTooLargeUpdate, tojson(stats));

    // This insert will land in a new bucket due to Bucket::kLargeMeasurementsMaxBucketSize being
    // exceeded by the first measurement.
    assert.commandWorked(coll.insert(measurement1, {ordered: isOrderedWrite}));

    stats = coll.stats().timeseries;
    assert.eq(2, stats.numBucketInserts, tojson(stats));
    assert.eq(0, stats.numBucketUpdates, tojson(stats));
    assert.eq(1, stats.numBucketsOpenedDueToMetadata, tojson(stats));
    assert.eq(1, stats.numBucketsClosedDueToSize, tojson(stats));
    assert.eq(0, stats.numBucketDocumentsTooLargeInsert, tojson(stats));
    assert.eq(0, stats.numBucketDocumentsTooLargeUpdate, tojson(stats));

    // This insert is not insertable, due to the 3x inflation of metric size + large meta,
    // this measurement is too large for bucket update and bucket insert.
    assert.commandFailedWithCode(coll.insert(measurement2, {ordered: isOrderedWrite}),
                                 ErrorCodes.BSONObjectTooLarge);

    stats = coll.stats().timeseries;
    assert.eq(2, stats.numBucketInserts, tojson(stats));
    assert.eq(0, stats.numBucketUpdates, tojson(stats));
    assert.eq(1, stats.numBucketsOpenedDueToMetadata, tojson(stats));
    assert.eq(2, stats.numBucketsClosedDueToSize, tojson(stats));
    // The failed ordered write retries as unordered and thus makes 2
    // unsuccessful attempts.
    assert.eq(isOrderedWrite ? 2 : 1, stats.numBucketDocumentsTooLargeInsert, tojson(stats));
    assert.eq(0, stats.numBucketDocumentsTooLargeUpdate, tojson(stats));

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
