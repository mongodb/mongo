/**
 * Bucket Insert: Measurements that are uninsertable due to exceeding the BSON size limit when a
 * bucket insert is generated to accommodate one measurement.
 *
 * Importantly, this controlled test checks collStats.
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

    const largeMeta = "a".repeat(16 * 1024 * 1024 + 1);
    const timestamp = ISODate("2025-01-01T12:00:00Z");
    const measurement1 = {};
    measurement1[timeField] = timestamp;
    measurement1[metaField] = largeMeta;
    measurement1["a"] = 1;

    const smallMeta = "5";
    const bigStr = "a".repeat(60000);
    const measurement2 = {};
    for (let i = 0; i < 100; ++i) {
        measurement2[i.toString()] = bigStr;
    }
    measurement2[timeField] = timestamp;
    measurement2[metaField] = smallMeta;

    // Insert Measurement

    jsTestLog("insert1");
    assert.commandFailedWithCode(coll.insert(measurement1, {ordered: isOrderedWrite}),
                                 ErrorCodes.BSONObjectTooLarge);

    let stats = coll.stats().timeseries;
    assert.eq(0, stats.numBucketInserts, tojson(stats));
    assert.eq(0, stats.numBucketUpdates, tojson(stats));
    assert.eq(isOrderedWrite ? 2 : 5, stats.numBucketsOpenedDueToMetadata, tojson(stats));
    assert.eq(0, stats.numBucketsClosedDueToSize, tojson(stats));
    // The failed ordered write retries as unordered and thus makes 2
    // unsuccessful attempts.
    assert.eq(isOrderedWrite ? 2 : 5, stats.numBucketDocumentsTooLargeInsert, tojson(stats));
    assert.eq(0, stats.numBucketDocumentsTooLargeUpdate, tojson(stats));

    jsTestLog("insert2");
    assert.commandFailedWithCode(coll.insert(measurement2, {ordered: isOrderedWrite}),
                                 ErrorCodes.BSONObjectTooLarge);

    stats = coll.stats().timeseries;
    assert.eq(0, stats.numBucketInserts, tojson(stats));
    assert.eq(0, stats.numBucketUpdates, tojson(stats));
    assert.eq(isOrderedWrite ? 4 : 6, stats.numBucketsOpenedDueToMetadata, tojson(stats));
    assert.eq(0, stats.numBucketsClosedDueToSize, tojson(stats));
    // The failed ordered write retries as unordered and thus makes 2
    // unsuccessful attempts.
    assert.eq(isOrderedWrite ? 4 : 6, stats.numBucketDocumentsTooLargeInsert, tojson(stats));
    assert.eq(0, stats.numBucketDocumentsTooLargeUpdate, tojson(stats));

    // Check Results
    // TODO(SERVER-108699): Remove this check.

    let buckets = bucketsColl.find().toArray();
    for (let i = 0; i < buckets.length; i++) {
        let bucketDocSize = Object.bsonsize(buckets[i]);
        assert.lte(bucketDocSize, 16 * 1024 * 1024);
    }

    coll.drop();
    // Stats do not reset on v7.0 when a collection drops. Thus, many checks are path-dependent
    // without a reboot.
}

runTest(/*isOrderedWrite=*/ true);
runTest(/*isOrderedWrite=*/ false);

rst.stopSet();
