/**
 * Test that inserting measurements out of order into a bucket will promote it to a v3 bucket,
 * meaning that it is compressed but that it no longer enforces sorting on time.
 *
 * @tags: [
 * # We need a time-series collection.
 * requires_timeseries,
 * requires_fcv_80,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod();

const dbName = "promoting_buckets";
const collName = "ts";
const timeFieldName = "time";

const testDB = conn.getDB(dbName);
const coll = testDB[collName];

coll.drop();
assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

const measurements = [
    {_id: 0, [timeFieldName]: ISODate("2024-02-15T10:10:10.000Z"), a: 1},
    {_id: 1, [timeFieldName]: ISODate("2024-02-15T10:10:20.000Z"), a: 2}
];

// Insert first measurement.
assert.commandWorked(coll.insert(measurements[1]));

// Ensure that there is only one bucket, and that its control version is v2.
assert.eq(getTimeseriesCollForRawOps(testDB, coll).find().rawData().length(), 1);
assert.eq(getTimeseriesCollForRawOps(testDB, coll)
              .find({"control.version": TimeseriesTest.BucketVersion.kCompressedSorted})
              .rawData()
              .length(),
          1);
// Ensure that we have not yet incremented our metric tracking the number of buckets promoted
// from v2 to v3.
let stats = assert.commandWorked(coll.stats());
assert.eq(stats.timeseries['numCompressedBucketsConvertedToUnsorted'], 0);

// Insert second measurement, which will cause the measurements to be out of order time wise
// and should cause the bucket to be promoted to v3.
assert.commandWorked(coll.insert(measurements[0]));

// Verify that the bucket's version has been updated to v3.
assert.eq(getTimeseriesCollForRawOps(testDB, coll).find().rawData().length(), 1);
assert.gte(getTimeseriesCollForRawOps(testDB, coll)
               .find({"control.version": TimeseriesTest.BucketVersion.kCompressedUnsorted})
               .rawData()
               .length(),
           1);

// Check that the numCompressedBucketsConvertedToUnsorted metric is properly incremented.
stats = assert.commandWorked(coll.stats());
assert.eq(stats.timeseries['numCompressedBucketsConvertedToUnsorted'], 1);

// Verify that the bucket's measurements are unpacked out of order.
let documents = coll.find().toArray();
assert.eq(documents.length, 2);
assert.docEq(documents[0], measurements[1]);
assert.docEq(documents[1], measurements[0]);

MongoRunner.stopMongod(conn);
