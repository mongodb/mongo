/**
 * Tests that, after downgrading, any v3 buckets that we encounter are treated as v2 buckets and do
 * not cause queries that unpack the buckets to fail.
 */

import "jstests/multiVersion/libs/multi_rs.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const latestVersion = "latest";
const oldVersion = "7.0";
const collName = "tsColl";
const bucketsCollName = "system.buckets." + collName;
const metaFieldName = "metaField";
const timeFieldName = "ts";

const measurements = [
    {_id: 0, [metaFieldName]: 'a', [timeFieldName]: ISODate("2024-10-30T10:10:00Z"), "value": 1},
    {_id: 1, [metaFieldName]: 'a', [timeFieldName]: ISODate("2024-10-30T10:10:20Z"), "value": 2},
    {_id: 2, [metaFieldName]: 'a', [timeFieldName]: ISODate("2024-10-30T10:10:10Z"), "value": 3},
];

const rst = new ReplSetTest({nodes: [{binVersion: latestVersion}]});
rst.startSet();
rst.initiate();

let testDB = rst.getPrimary().getDB(jsTestName());
let coll = testDB[collName];
let bucketsColl = testDB[bucketsCollName];
coll.drop();

assert.commandWorked(testDB.createCollection(
    collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

// Insert our first measurements.
assert.commandWorked(coll.insertMany([measurements[0], measurements[1]]));

// Ensure that there is only one bucket, and that its control version is v2.
assert.eq(bucketsColl.find().itcount(), 1);
assert.eq(
    bucketsColl.find({"control.version": TimeseriesTest.BucketVersion.kCompressedSorted}).itcount(),
    1);

// Now insert another measurement, which has a timestamp that is older than the last inserted
// document - this should cause the measurements to be out of order and should cause the bucket to
// be promoted to v3.
assert.commandWorked(coll.insert(measurements[2]));

// Verify that we still only have one bucket, and that its version is now v3.
assert.eq(bucketsColl.find().itcount(), 1);
assert.eq(bucketsColl.find({"control.version": TimeseriesTest.BucketVersion.kCompressedUnsorted})
              .itcount(),
          1);
// Let's unpack the bucket and read its documents.
let documents = coll.find().sort({_id: 1}).toArray();
assert.eq(documents.length, 3);
assert.docEq(documents[0], measurements[0]);
assert.docEq(documents[1], measurements[1]);
assert.docEq(documents[2], measurements[2]);

// Now, let's downgrade and check that we are still able to read and unpacked v3 buckets.
assert.commandWorked(rst.getPrimary().getDB("admin").runCommand(
    {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
rst.upgradeSet({binVersion: oldVersion});

testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB[collName];
bucketsColl = testDB[bucketsCollName];

// Verify that we still have our v3 bucket.
assert.eq(bucketsColl.find().itcount(), 1);
assert.eq(bucketsColl.find({"control.version": TimeseriesTest.BucketVersion.kCompressedUnsorted})
              .itcount(),
          1);
// Even though we've downgraded to 7.0, unpacking a v3 bucket should not cause a uassert - it should
// be read as and treated as a v3 bucket.
documents = coll.find().sort({_id: 1}).toArray();
// Let's unpack the bucket and read its documents.
assert.eq(documents.length, 3);
assert.docEq(documents[0], measurements[0]);
assert.docEq(documents[1], measurements[1]);
assert.docEq(documents[2], measurements[2]);

rst.stopSet();
