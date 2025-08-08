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
    const bucketsColl = testDB["system.buckets." + collName];

    var doc = {
        t: ISODate(),
        m: "meta",
        a: "foo",
    };
    assert.commandWorked(coll.insert(doc));
    assert.eq(1, coll.find({"m": "meta"}).toArray().length);
    assert.eq(1, bucketsColl.find({}).toArray().length);

    // new measurement, count buckets
    doc.a = "bar";
    assert.commandWorked(coll.insert(doc));
    assert.eq(2, coll.find({"m": "meta"}).toArray().length);
    const buckets = bucketsColl.find({"meta": "meta"}).toArray().length;

    // new measurement, because schema changed, we have a new bucket
    doc.a = 1;
    assert.commandWorked(coll.insert(doc));
    assert.eq(3, coll.find({"m": "meta"}).toArray().length);
    assert.eq(buckets + 1, bucketsColl.find({"meta": "meta"}).toArray().length);
})();

/**
 * Test 2: Test with very large docs, checks bug discovered in SERVER-107361
 */
(function largeDocInsertMixedSchemaTest() {
    assert.commandWorked(testDB.runCommand({drop: collName}));
    assert.commandWorked(
        testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
    const coll = testDB[collName];
    const bucketsColl = testDB["system.buckets." + collName];

    var doc = {t: ISODate(), m: "meta", a: 1, payload: "small"};
    assert.commandWorked(coll.insert(doc));
    assert.eq(1, coll.find({"m": "meta"}).toArray().length);
    assert.eq(1, bucketsColl.find({}).toArray().length);

    // new measurement, count buckets
    doc.a = 2;
    assert.commandWorked(coll.insert(doc));
    assert.eq(2, coll.find({"m": "meta"}).toArray().length);
    const buckets = bucketsColl.find({"meta": "meta"}).toArray().length;

    // new measurement, because schema changed, we have a new bucket
    // previous bug would skip mixed schema check due to handling of large measurements
    doc.a = "foo";
    doc.payload = 'A'.repeat(130000);
    assert.commandWorked(coll.insert(doc));
    assert.eq(3, coll.find({"m": "meta"}).toArray().length);
    assert.eq(buckets + 1, bucketsColl.find({"meta": "meta"}).toArray().length);
})();
