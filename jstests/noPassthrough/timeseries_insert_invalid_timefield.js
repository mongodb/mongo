/**
 * Tests that a time-series collection rejects documents with invalid timeField values
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const conn = MongoRunner.runMongod();

if (!TimeseriesTest.timeseriesCollectionsEnabled(conn)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection('t');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
coll.drop();

const timeFieldName = 'time';
const metaFieldName = 'meta';

assert.commandWorked(testDB.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

// first test a good doc just in case
const goodDocs = [
    {
        _id: 0,
        time: ISODate("2020-11-26T00:00:00.000Z"),
        meta: "A",
        data: true,
    },
    {
        _id: 1,
        time: ISODate("2020-11-27T00:00:00.000Z"),
        meta: "A",
        data: true,
    }
];
assert.commandWorked(coll.insert(goodDocs[0]));
assert.eq(1, coll.count());
assert.docEq(coll.find().toArray(), [goodDocs[0]]);

// now make sure we reject if timeField is missing or isn't a valid BSON datetime
let mixedDocs = [{meta: "B", data: true}, goodDocs[1], {time: "invalid", meta: "B", data: false}];
assert.commandFailedWithCode(coll.insert(mixedDocs, {ordered: false}), ErrorCodes.BadValue);
assert.eq(coll.count(), 2);
assert.docEq(coll.find().toArray(), goodDocs);
assert.eq(null, coll.findOne({meta: mixedDocs[0].meta}));
assert.eq(null, coll.findOne({meta: mixedDocs[2].meta}));

MongoRunner.stopMongod(conn);
})();
