/**
 * Tests that time-series inserts respect {ordered: true}.
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');
load('jstests/libs/fail_point_util.js');

const conn = MongoRunner.runMongod();

if (!TimeseriesTest.timeseriesCollectionsEnabled(conn)) {
    jsTestLog('Skipping test because the time-series collection feature flag is disabled');
    MongoRunner.stopMongod(conn);
    return;
}

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
assert.docEq(res.getWriteErrors()[0].getOperation(), docs[3]);

// The document that successfully inserted should go into a new bucket due to the failed insert on
// the existing bucket.
assert.docEq(coll.find().sort({_id: 1}).toArray(), docs.slice(0, 3));
assert.eq(bucketsColl.count(),
          3,
          'Expected two buckets but found: ' + tojson(bucketsColl.find().toArray()));

fp1.off();
fp2.off();

// The documents should go into two new buckets due to the failed insert on the existing bucket.
assert.commandWorked(coll.insert(docs.slice(3), {ordered: true}));
assert.docEq(coll.find().sort({_id: 1}).toArray(), docs);
assert.eq(bucketsColl.count(),
          5,
          'Expected four buckets but found: ' + tojson(bucketsColl.find().toArray()));

MongoRunner.stopMongod(conn);
})();