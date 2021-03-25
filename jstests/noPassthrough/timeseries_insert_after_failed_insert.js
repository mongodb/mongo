/**
 * Tests that a failed time-series insert does not leave behind any invalid state.
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
    {_id: 0, meta: 'fail', [timeFieldName]: ISODate()},
    {_id: 1, meta: 'fail', [timeFieldName]: ISODate()},
];

const fp = configureFailPoint(conn, 'failTimeseriesInsert', {metadata: 'fail'});
assert.commandFailed(coll.insert(docs[0], {ordered: false}));
fp.off();

// Insert a document that belongs in the same bucket that the failed insert woulld have gone into.
assert.commandWorked(coll.insert(docs[1], {ordered: false}));

// There should not be any leftover state from the failed insert.
assert.docEq(coll.find().toArray(), [docs[1]]);
const buckets = bucketsColl.find().sort({['control.min.' + timeFieldName]: 1}).toArray();
jsTestLog('Checking buckets: ' + tojson(buckets));
assert.eq(buckets.length, 1);
assert.eq(buckets[0].control.min._id, docs[1]._id);

MongoRunner.stopMongod(conn);
})();