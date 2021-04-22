/**
 * Tests that time-series inserts respect {ordered: false}.
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
    {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: 0},
    {_id: 2, [timeFieldName]: ISODate(), [metaFieldName]: 0},
    {_id: 3, [timeFieldName]: ISODate(), [metaFieldName]: 1},
    {_id: 4, [timeFieldName]: ISODate(), [metaFieldName]: 1},
];

assert.commandWorked(coll.insert(docs[0]));

const fp = configureFailPoint(conn, 'failUnorderedTimeseriesInsert', {metadata: 0});

// Insert two documents that would go into the existing bucket and two documents that go into a new
// bucket.
const res = assert.commandFailed(coll.insert(docs.slice(1), {ordered: false}));

jsTestLog('Checking insert result: ' + tojson(res));
assert.eq(res.nInserted, 2);
assert.eq(res.getWriteErrors().length, docs.length - res.nInserted - 1);
for (let i = 0; i < res.getWriteErrors().length; i++) {
    assert.eq(res.getWriteErrors()[i].index, i);
    assert.docEq(res.getWriteErrors()[i].getOperation(), docs[i + 1]);
}

assert.docEq(coll.find().sort({_id: 1}).toArray(), [docs[0], docs[3], docs[4]]);
assert.eq(bucketsColl.count(),
          2,
          'Expected two buckets but found: ' + tojson(bucketsColl.find().toArray()));

fp.off();

// The documents should go into two new buckets due to the failed insert on the existing bucket.
assert.commandWorked(coll.insert(docs.slice(1, 3), {ordered: false}));
assert.docEq(coll.find().sort({_id: 1}).toArray(), docs);
assert.eq(bucketsColl.count(),
          3,
          'Expected three buckets but found: ' + tojson(bucketsColl.find().toArray()));

MongoRunner.stopMongod(conn);
})();