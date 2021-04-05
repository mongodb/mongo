/**
 * Tests that direct updates to a timeseries bucket collection close the bucket, preventing further
 * inserts to land in that bucket.
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

const conn = MongoRunner.runMongod();

if (!TimeseriesTest.timeseriesCollectionsEnabled(conn)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

const collName = 'test';

const timeFieldName = 'time';
const times = [
    ISODate('2021-01-01T01:00:00Z'),
    ISODate('2021-01-01T01:10:00Z'),
    ISODate('2021-01-01T01:20:00Z')
];
let docs = [
    {_id: 0, [timeFieldName]: times[0]},
    {_id: 1, [timeFieldName]: times[1]},
    {_id: 2, [timeFieldName]: times[2]}
];

const coll = testDB.getCollection(collName);
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
coll.drop();

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

assert.commandWorked(coll.insert(docs[0]));
assert.docEq(coll.find().sort({_id: 1}).toArray(), docs.slice(0, 1));

let buckets = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(buckets.length, 1);
assert.eq(buckets[0].control.min[timeFieldName], times[0]);
assert.eq(buckets[0].control.max[timeFieldName], times[0]);

let modified = buckets[0];
modified.control.closed = true;
let updateResult = assert.commandWorked(bucketsColl.update({_id: buckets[0]._id}, modified));
assert.eq(updateResult.nMatched, 1);
assert.eq(updateResult.nModified, 1);

buckets = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(buckets.length, 1);
assert.eq(buckets[0].control.min[timeFieldName], times[0]);
assert.eq(buckets[0].control.max[timeFieldName], times[0]);
assert(buckets[0].control.closed);

assert.commandWorked(coll.insert(docs[1]));
assert.docEq(coll.find().sort({_id: 1}).toArray(), docs.slice(0, 2));

buckets = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(buckets.length, 2);
assert.eq(buckets[1].control.min[timeFieldName], times[1]);
assert.eq(buckets[1].control.max[timeFieldName], times[1]);

let fpInsert = configureFailPoint(conn, "hangTimeseriesInsertBeforeCommit");
let awaitInsert = startParallelShell(
    funWithArgs(function(dbName, collName, doc) {
        assert.commandWorked(db.getSiblingDB(dbName).getCollection(collName).insert(doc));
    }, dbName, coll.getName(), docs[2]), conn.port);

fpInsert.wait();

modified = buckets[1];
modified.control.closed = true;
updateResult = assert.commandWorked(bucketsColl.update({_id: buckets[1]._id}, modified));
assert.eq(updateResult.nMatched, 1);
assert.eq(updateResult.nModified, 1);

fpInsert.off();
awaitInsert();

assert.docEq(coll.find().sort({_id: 1}).toArray(), docs.slice(0, 3));

buckets = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(buckets.length, 3);
assert.eq(buckets[1].control.min[timeFieldName], times[1]);
assert.eq(buckets[1].control.max[timeFieldName], times[1]);
assert(buckets[1].control.closed);
assert.eq(buckets[2].control.min[timeFieldName], times[2]);
assert.eq(buckets[2].control.max[timeFieldName], times[2]);

MongoRunner.stopMongod(conn);
})();
