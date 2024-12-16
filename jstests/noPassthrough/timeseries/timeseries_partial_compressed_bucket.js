/**
 * Create a compressed bucket with uncompressed data fields and attempt to reopen for an insert.
 * Two types of corruption:
 * - adding an additional field that is uncompressed
 * - changing an existing compressed field to uncompressed
 *
 * @tags: [
 *     requires_timeseries,
 *     requires_fcv_80,
 * ]
 */

// This test intentionally corrupts a bucket, so disable testing diagnostics.
TestData.testingDiagnosticsEnabled = false;

const conn = MongoRunner.runMongod();

const collName = jsTestName();
const db = conn.getDB(collName);
const coll = db.getCollection(collName);
const bucketsColl = db.getCollection("system.buckets." + collName);

const timeFieldName = 't';

const measurements = [
    {_id: 0, [timeFieldName]: ISODate("2024-02-15T10:10:10.000Z"), a: 0, b: 0},
    {_id: 1, [timeFieldName]: ISODate("2024-02-15T08:10:20.000Z"), a: 1, b: 1},
    {_id: 2, [timeFieldName]: ISODate("2024-02-15T10:10:20.000Z"), a: 2, b: 2},
    {_id: 3, [timeFieldName]: ISODate("2024-02-15T10:10:10.000Z"), a: 3, b: 3},
];

assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

jsTestLog("Insert first measurement");
assert.commandWorked(coll.insert(measurements[0]));

let bucketId = bucketsColl.find()[0]._id;

jsTestLog("Insert second measurement, which archives the prior bucket due to kTimeBackward");
assert.commandWorked(coll.insert(measurements[1]));

let stats = assert.commandWorked(coll.stats());
assert.eq(stats.timeseries.numBucketsArchivedDueToTimeBackward, 1, tojson(stats));

jsTestLog("Add uncompressed data field to bucket, thus corrupting a compressed bucket.");
let res = assert.commandWorked(bucketsColl.updateOne(
    {_id: bucketId}, {$set: {"data.c.1": 1, "control.min.c": 1, "control.max.c": 1}}));
assert.eq(res.modifiedCount, 1);

jsTestLog(
    "Insert third measurement. This will attempt to re-open the corrupted bucket, but should then freeze it and insert into a new bucket.");
assert.commandWorked(coll.insert(measurements[2]));

stats = assert.commandWorked(coll.stats());
assert.eq(stats.timeseries.numBucketInserts, 3, tojson(stats.timeseries));
assert.eq(stats.timeseries.numCommits, 3, tojson(stats.timeseries));
assert.eq(stats.timeseries.numBucketsReopened, 0, tojson(stats.timeseries));
assert.eq(stats.timeseries.numBucketsFrozen, 1, tojson(stats.timeseries));
assert.eq(stats.timeseries.numBucketQueriesFailed, 0, tojson(stats.timeseries));
assert.eq(stats.timeseries.numBucketReopeningsFailed, 1, tojson(stats.timeseries));

jsTestLog(
    "Replace compressed data field with an uncompressed field, thus corrupting a compressed bucket.");
bucketId = bucketsColl.find({"control.min.a": 2})[0]._id;
res = assert.commandWorked(bucketsColl.updateOne({_id: bucketId}, {$set: {"data.b": {"0": 1}}}));
assert.eq(res.modifiedCount, 1);

jsTestLog(
    "Insert fourth measurement. This will attempt to re-open the second corrupted bucket, but should then freeze it and insert into a new bucket.");
assert.commandWorked(coll.insert(measurements[3]));

stats = assert.commandWorked(coll.stats());
assert.eq(stats.timeseries.numBucketInserts, 4, tojson(stats.timeseries));
assert.eq(stats.timeseries.numCommits, 4, tojson(stats.timeseries));
assert.eq(stats.timeseries.numBucketsReopened, 0, tojson(stats.timeseries));
assert.eq(stats.timeseries.numBucketsFrozen, 2, tojson(stats.timeseries));
assert.eq(stats.timeseries.numBucketQueriesFailed, 0, tojson(stats.timeseries));
assert.eq(stats.timeseries.numBucketReopeningsFailed, 2, tojson(stats.timeseries));

// Skip validation due to the corrupt buckets.
MongoRunner.stopMongod(conn, null, {skipValidation: true});
