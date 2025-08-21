/**
 * Tests directly inserting a time-series bucket with mixed schema.
 *
 * @tags: [
 *   requires_timeseries,
 *   # TODO(SERVER-108445) Reenable this test
 *   multiversion_incompatible,
 * ]
 */

import {getTimeseriesCollForRawOps} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const testDB = db.getSiblingDB(jsTestName());
const collName = "ts";

/**
 * Test 1: Basic insert of mixed schema, check bucket counts
 */
TimeseriesTest.run((insert) => {
    assert.commandWorked(testDB.runCommand({drop: collName}));
    assert.commandWorked(
        testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
    const coll = testDB[collName];

    var doc = {
        t: ISODate(),
        m: "meta",
        a: "foo",
    };
    assert.commandWorked(insert(coll, doc));
    assert.eq(1, coll.find({"m": "meta"}).toArray().length);
    assert.eq(1, getTimeseriesCollForRawOps(coll).find({}).rawData().toArray().length);

    // new measurement, same bucket
    doc.a = "bar";
    assert.commandWorked(insert(coll, doc));
    assert.eq(2, coll.find({"m": "meta"}).toArray().length);
    assert.eq(1,
              getTimeseriesCollForRawOps(coll).find({"meta": "meta"}).rawData().toArray().length);

    // new measurement, because schema changed, we have a new bucket
    doc.a = 1;
    assert.commandWorked(insert(coll, doc));
    assert.eq(3, coll.find({"m": "meta"}).toArray().length);
    assert.eq(2,
              getTimeseriesCollForRawOps(coll).find({"meta": "meta"}).rawData().toArray().length);
    var schemaChangedBucket =
        getTimeseriesCollForRawOps(coll).find({"control.min.a": 1}).rawData().toArray();
    assert.eq(1, schemaChangedBucket.length);
    var schemaChangedBucketId = schemaChangedBucket[0]._id;

    // new measurement, schema changed back, necessitating a different bucket
    doc.a = "bam";
    assert.commandWorked(insert(coll, doc));
    assert.eq(4, coll.find({"m": "meta"}).toArray().length);
    // New measurement may go into new bucket, or prev bucket (reopened), but should not go into
    // the recently created bucket.
    schemaChangedBucket =
        getTimeseriesCollForRawOps(coll).find({"_id": schemaChangedBucketId}).rawData().toArray();
    assert.eq(1, schemaChangedBucket.length);
    assert.eq(1, schemaChangedBucket[0].control.count);
});

/**
 * Test 2: Test with very large docs, checks bug discovered in SERVER-107361
 */
TimeseriesTest.run((insert) => {
    assert.commandWorked(testDB.runCommand({drop: collName}));
    assert.commandWorked(
        testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
    const coll = testDB[collName];

    var doc = {t: ISODate(), m: "meta", a: 1, payload: "small"};
    assert.commandWorked(insert(coll, doc));
    assert.eq(1, coll.find({"m": "meta"}).toArray().length);
    assert.eq(1, getTimeseriesCollForRawOps(coll).find({}).rawData().toArray().length);

    // new measurement, because schema changed, we have a new bucket
    // previous bug would skip mixed schema check due to handling of large measurements
    doc.a = "foo";
    doc.payload = 'A'.repeat(130000);
    assert.commandWorked(insert(coll, doc));
    assert.eq(2, coll.find({"m": "meta"}).toArray().length);
    assert.eq(2,
              getTimeseriesCollForRawOps(coll).find({"meta": "meta"}).rawData().toArray().length);
    var schemaChangedBucket =
        getTimeseriesCollForRawOps(coll).find({"control.min.a": "foo"}).rawData().toArray();
    assert.eq(1, schemaChangedBucket.length);
    var schemaChangedBucketId = schemaChangedBucket[0]._id;

    // new measurement, schema changed back, necessitating a different bucket
    doc.a = 2;
    doc.payload = "small";
    assert.commandWorked(insert(coll, doc));
    assert.eq(3, coll.find({"m": "meta"}).toArray().length);
    // New measurement may go into new bucket, or prev bucket (reopened), but should not go into
    // the recently created bucket.
    schemaChangedBucket =
        getTimeseriesCollForRawOps(coll).find({"_id": schemaChangedBucketId}).rawData().toArray();
    assert.eq(1, schemaChangedBucket.length);
    assert.eq(1, schemaChangedBucket[0].control.count);
});
