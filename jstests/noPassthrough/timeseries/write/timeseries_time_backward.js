/**
 * Insert measurements backwards in time to test bucket archiving.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_fcv_80,
 * ]
 */

jsTestLog("Running " + jsTestName());

const conn = MongoRunner.runMongod({});
const db = conn.getDB("test");

const timeFieldName = "t";
const metaFieldName = "m";

assert.commandWorked(db.createCollection(
    jsTestName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

const coll = db.getCollection(jsTestName());
const bucketsColl = db.getCollection("system.buckets." + jsTestName());

let times = [
    ISODate("2024-04-08T00:10:00.000Z"),
    ISODate("2024-04-08T00:09:00.000Z"),
    ISODate("2024-04-08T00:08:00.000Z"),
    ISODate("2024-04-08T00:07:00.000Z"),
    ISODate("2024-04-08T00:06:00.000Z"),
    ISODate("2024-04-08T00:05:00.000Z"),
    ISODate("2024-04-08T00:04:00.000Z"),
    ISODate("2024-04-08T00:03:00.000Z"),
    ISODate("2024-04-08T00:02:00.000Z"),
    ISODate("2024-04-08T00:01:00.000Z"),
];

for (let i = 0; i < times.length; i++) {
    assert.commandWorked(coll.insert({[timeFieldName]: times[i], [metaFieldName]: 1, x: 1}));
}

let stats = coll.stats().timeseries;
jsTestLog(stats);
assert.eq(stats["bucketCount"], 10);
assert.eq(stats["numBucketInserts"], 10);
assert.eq(stats["numBucketUpdates"], 0);
assert.eq(stats["numBucketsArchivedDueToTimeBackward"], 9);
assert.eq(stats["numBucketsOpenedDueToMetadata"], 1);
assert.eq(stats["numCommits"], 10);
assert.eq(stats["numMeasurementsCommitted"], 10);
assert.eq(stats["numBucketQueriesFailed"], 10);
assert.eq(stats["numCompressedBucketsConvertedToUnsorted"], 0);

MongoRunner.stopMongod(conn);
