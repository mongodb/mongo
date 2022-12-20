/**
 * Tests that time-series inserts respect {ordered: true}.
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');
load('jstests/libs/fail_point_util.js');

const conn = MongoRunner.runMongod();

const testDB = conn.getDB(jsTestName());
const coll = testDB.getCollection('t');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

const timeFieldName = 'time';
const metaFieldName = 'meta';

coll.drop();
assert.commandWorked(testDB.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

const docs = [
    {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: 0},
    {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: 1},
    {_id: 2, [timeFieldName]: ISODate(), [metaFieldName]: 0},
    {_id: 5, [timeFieldName]: ISODate(), [metaFieldName]: 1},
    {_id: 6, [timeFieldName]: ISODate(), [metaFieldName]: 2},
];

assert.commandWorked(coll.insert(docs.slice(0, 2)));

const fp1 = configureFailPoint(conn, 'failAtomicTimeseriesWrites');
const fp2 = configureFailPoint(conn, 'failUnorderedTimeseriesInsert', {metadata: 1});

const res = assert.commandFailed(coll.insert(docs.slice(2), {ordered: true}));

jsTestLog('Checking insert result: ' + tojson(res));
assert.eq(res.nInserted, 1);
assert.eq(res.getWriteErrors().length, 1);
assert.eq(res.getWriteErrors()[0].index, 1);
assert.docEq(docs[3], res.getWriteErrors()[0].getOperation());

// The document that successfully inserted should go into a new bucket due to the failed insert on
// the existing bucket.
assert.docEq(docs.slice(0, 3), coll.find().sort({_id: 1}).toArray());
// If we allow bucket reopening, we will save out on opening another bucket.
let expectedBucketCount = (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(testDB)) ? 2 : 3;
assert.eq(bucketsColl.count(),
          expectedBucketCount,
          'Expected ' + expectedBucketCount +
              ' buckets but found: ' + tojson(bucketsColl.find().toArray()));

fp1.off();
fp2.off();

// The documents should go into two new buckets due to the failed insert on the existing bucket.
assert.commandWorked(coll.insert(docs.slice(3), {ordered: true}));
assert.docEq(docs, coll.find().sort({_id: 1}).toArray());
// If we allow bucket reopening, we will save out on opening new buckets. Resulting in one bucket
// per unique meta field.
expectedBucketCount = (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(testDB)) ? 3 : 5;
assert.eq(bucketsColl.count(),
          expectedBucketCount,
          'Expected ' + expectedBucketCount +
              ' buckets but found: ' + tojson(bucketsColl.find().toArray()));

MongoRunner.stopMongod(conn);
})();
