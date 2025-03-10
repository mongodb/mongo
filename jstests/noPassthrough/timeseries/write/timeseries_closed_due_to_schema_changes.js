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
    assert.commandWorked(coll.insert({[timeFieldName]: t0, [metaFieldName]: 0, x: typeList[i]}));
    assert.eq(coll.stats().timeseries.numBucketsClosedDueToSchemaChange, i);
}

MongoRunner.stopMongod(conn);
