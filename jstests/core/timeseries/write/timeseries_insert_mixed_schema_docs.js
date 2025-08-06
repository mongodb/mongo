/**
 * Tests directly inserting a time-series bucket with mixed schema.
 *
 * @tags: [
 *   # $listCatalog does not include the tenant prefix in its results.
 *   command_not_supported_in_serverless,
 *   requires_timeseries,
 *   does_not_support_viewless_timeseries_yet,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesBucketsColl} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const testDB = db.getSiblingDB(jsTestName());
const collName = "ts";

/**
 * Test 1: Basic insert of mixed schema, check bucket counts
 */
(function basicInsertMixedSchemaTest() {
    assert.commandWorked(testDB.runCommand({drop: collName}));
    assert.commandWorked(
        testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
    const coll = testDB[collName];

    var doc = {
        t: ISODate(),
        m: "meta",
        a: "foo",
    };
    assert.commandWorked(coll.insert(doc));
    assert.eq(1, coll.find({"m": "meta"}).toArray().length);
    assert.eq(1, getTimeseriesBucketsColl(coll).find({}).toArray().length);

    // new measurement, same bucket
    doc.a = "bar";
    assert.commandWorked(coll.insert(doc));
    assert.eq(2, coll.find({"m": "meta"}).toArray().length);
    assert.eq(1, getTimeseriesBucketsColl(coll).find({"meta": "meta"}).toArray().length);

    // new measurement, because schema changed, we have a new bucket
    doc.a = 1;
    assert.commandWorked(coll.insert(doc));
    assert.eq(3, coll.find({"m": "meta"}).toArray().length);
    assert.eq(2, getTimeseriesBucketsColl(coll).find({"meta": "meta"}).toArray().length);

    // new measurement, schema changed back, necessitating a third bucket
    doc.a = "bam";
    assert.commandWorked(coll.insert(doc));
    assert.eq(4, coll.find({"m": "meta"}).toArray().length);
    // New measurement may go into new bucket, or prev bucket (reopened), but should not go into
    // the recently created bucket.
    assert.eq(1, getTimeseriesBucketsColl(coll).find({"meta": "meta"}).toArray()[1].control.count);
})();

/**
 * Test 2: Test with very large docs, checks bug discovered in SERVER-107361
 */
(function largeDocInsertMixedSchemaTest() {
    assert.commandWorked(testDB.runCommand({drop: collName}));
    assert.commandWorked(
        testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
    const coll = testDB[collName];

    var doc = {t: ISODate(), m: "meta", a: 1, payload: "small"};
    assert.commandWorked(coll.insert(doc));
    assert.eq(1, coll.find({"m": "meta"}).toArray().length);
    assert.eq(1, getTimeseriesBucketsColl(coll).find({}).toArray().length);

    // new measurement, because schema changed, we have a new bucket
    // previous bug would skip mixed schema check due to handling of large measurements
    doc.a = "foo";
    doc.payload = 'A'.repeat(130000);
    assert.commandWorked(coll.insert(doc));
    assert.eq(2, coll.find({"m": "meta"}).toArray().length);
    assert.eq(2, getTimeseriesBucketsColl(coll).find({"meta": "meta"}).toArray().length);

    // new measurement, schema changed back, necessitating a third bucket
    doc.a = 2;
    doc.payload = "small";
    assert.commandWorked(coll.insert(doc));
    assert.eq(3, coll.find({"m": "meta"}).toArray().length);
    // New measurement may go into new bucket, or prev bucket (reopened), but should not go into
    // the recently created bucket.
    assert.eq(1, getTimeseriesBucketsColl(coll).find({"meta": "meta"}).toArray()[1].control.count);
})();
