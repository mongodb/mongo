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

const resetColl = function() {
    coll.drop();
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.contains(bucketsColl.getName(), testDB.getCollectionNames());
};
resetColl();

configureFailPoint(conn, 'failTimeseriesInsert', {metadata: 'fail'});

const docs = [
    {_id: 0, [timeFieldName]: ISODate()},
    {_id: 1, [timeFieldName]: ISODate()},
    {_id: 2, [timeFieldName]: ISODate()},
];

let res = assert.commandWorked(coll.insert(docs, {ordered: true}));
assert.eq(res.nInserted, 3, 'Invalid insert result: ' + tojson(res));
assert.docEq(coll.find().sort({_id: 1}).toArray(), docs);
resetColl();

docs[1][metaFieldName] = 'fail';
res = assert.commandFailed(coll.insert(docs, {ordered: true}));
jsTestLog('Checking insert result: ' + tojson(res));
assert.eq(res.nInserted, 1);
assert.eq(res.getWriteErrors().length, 1);
assert.eq(res.getWriteErrors()[0].index, 1);
assert.docEq(res.getWriteErrors()[0].getOperation(), docs[1]);
assert.docEq(coll.find().toArray(), [docs[0]]);

MongoRunner.stopMongod(conn);
})();