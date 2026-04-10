/**
 * Tests a repair mechanism for timeseries buckets that fail strict validation. A bucket that is rejected by
 * strict validation can still be decompressed by $_internalUnpackBucket in some cases. The repair uses an aggregation
 * pipeline with $_internalUnpackBucket to extract the measurements and $out to write them to a
 * new valid timeseries collection.
 *
 * This demonstrates a repair path that customers can use to recover measurements from bucket data
 * that was created before strict bucket validation was enforced.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

const timeField = "t";
const metaField = "m";

// Create a timeseries collection and insert two measurements with known values, this gives us a valid
// bucket to base the test on.
const sourceColl = db.source;
assert.commandWorked(db.createCollection(sourceColl.getName(), {timeseries: {timeField, metaField}}));

const measurements = [
    {[timeField]: ISODate("2024-01-16T20:48:00.000Z"), [metaField]: "sensor1", a: 10},
    {[timeField]: ISODate("2024-01-16T20:48:30.000Z"), [metaField]: "sensor1", a: 20},
];
assert.commandWorked(sourceColl.insertMany(measurements));

// Extract the compressed bucket that was created for these measurements.
const sourceBuckets = getTimeseriesCollForRawOps(db, sourceColl).find().rawData().toArray();
assert.eq(1, sourceBuckets.length, "Expected one bucket: " + tojson(sourceBuckets));
const validBucket = sourceBuckets[0];

// Build an invalid bucket by replacing the _id with an OID whose embedded timestamp does not match control.min.t. This bucket can no longer be inserted into a timeseries collection.
const wrongTimestampOID = ObjectId("63b0ec000000000000000000");
const invalidBucket = Object.assign({}, validBucket, {_id: wrongTimestampOID});

const verifyResult = db.runCommand({
    insert: getTimeseriesCollForRawOps(db, sourceColl).getName(),
    documents: [invalidBucket],
    ...getRawOperationSpec(db),
});
assert(
    verifyResult.writeErrors && verifyResult.writeErrors.length > 0,
    "Expected strict validation to reject bucket with mismatched OID timestamp: " + tojson(verifyResult),
);

// Insert the invalid bucket into a regular (non-timeseries) collection. Regular collections
// do not run timeseries bucket validation, so the insert succeeds unconditionally.
const stagingColl = db.staging;
assert.commandWorked(stagingColl.insertOne(invalidBucket));

// Repair the invalid bucket using the internal timeseries aggregation pipeline:
//   1. $_internalUnpackBucket decompresses the bucket into individual measurement documents.
//   2. $out writes those measurements to a new timeseries collection, creating valid buckets
//      with correct OID timestamps.
const outCollName = "repaired";
assert.commandWorked(
    db.runCommand({
        aggregate: stagingColl.getName(),
        pipeline: [
            {
                $_internalUnpackBucket: {
                    timeField,
                    metaField,
                    bucketMaxSpanSeconds: NumberInt(3600),
                },
            },
            {$out: {db: db.getName(), coll: outCollName, timeseries: {timeField, metaField}}},
        ],
        cursor: {},
    }),
);

// Verify that the repaired collection contains exactly the original measurements.
const repairedColl = db[outCollName];
const repairedDocs = repairedColl
    .find()
    .sort({[timeField]: 1})
    .toArray();
assert.eq(measurements.length, repairedDocs.length, tojson(repairedDocs));
for (let i = 0; i < measurements.length; i++) {
    assert.eq(measurements[i][timeField], repairedDocs[i][timeField], tojson(repairedDocs[i]));
    assert.eq(measurements[i][metaField], repairedDocs[i][metaField], tojson(repairedDocs[i]));
    assert.eq(measurements[i].a, repairedDocs[i].a, tojson(repairedDocs[i]));
}

// Confirm the repaired collection is a valid timeseries collection with no inconsistencies.
const validateResult = repairedColl.validate();
assert(validateResult.valid, tojson(validateResult));
assert.eq(0, validateResult.nNonCompliantDocuments, tojson(validateResult));

MongoRunner.stopMongod(conn);
