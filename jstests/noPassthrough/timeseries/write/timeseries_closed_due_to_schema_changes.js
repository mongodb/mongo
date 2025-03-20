/**
 * Test schema changes on buckets with various BSON types.
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

let t0 = ISODate("2024-04-08T10:00:00.000Z");

let typeList = [
    ObjectId("0102030405060708090A0B0C"),
    bsonObjToArray({"a": 1, "b": 2}),
    ISODate(),
    undefined,
    "MongoDB",
    NumberDecimal("9223372036854775807"),
    {a: 20},
    42,
];

for (let i = 0; i < typeList.length; i++) {
    // TODO(SERVER-102525): Add the following checks.
    // After the second measurement, we begin to reopen hard-closed buckets that were closed due to
    // schema change. Because none of the measurements are schema compatible, we will now have two
    // numBucketsClosedDueToSchemaChange; the previous open bucket and a query-based reopening
    // bucket.
    // let numBuckets = (i < 2) ? i : (2 * i - 1);
    // assert.eq(coll.stats().timeseries.numBucketsClosedDueToSchemaChange, numBuckets);
}

MongoRunner.stopMongod(conn);
