/**
 * Tests that direct removal in a timeseries bucket collection close the relevant bucket, preventing
 * further inserts from landing in that bucket.
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

let removeResult = assert.commandWorked(bucketsColl.remove({_id: buckets[0]._id}));
assert.eq(removeResult.nRemoved, 1);

buckets = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(buckets.length, 0);

assert.commandWorked(coll.insert(docs[1]));
assert.docEq(coll.find().sort({_id: 1}).toArray(), docs.slice(1, 2));

buckets = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(buckets.length, 1);
assert.eq(buckets[0].control.min[timeFieldName], times[1]);
assert.eq(buckets[0].control.max[timeFieldName], times[1]);

let fpInsert = configureFailPoint(conn, "hangTimeseriesInsertBeforeCommit");
let awaitInsert = startParallelShell(
    funWithArgs(function(dbName, collName, doc) {
        assert.commandWorked(db.getSiblingDB(dbName).getCollection(collName).insert(doc));
    }, dbName, coll.getName(), docs[2]), conn.port);

fpInsert.wait();

removeResult = assert.commandWorked(bucketsColl.remove({_id: buckets[0]._id}));
assert.eq(removeResult.nRemoved, 1);

fpInsert.off();
awaitInsert();

assert.docEq(coll.find().sort({_id: 1}).toArray(), docs.slice(2, 3));

buckets = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(buckets.length, 1);
assert.eq(buckets[0].control.min[timeFieldName], times[2]);
assert.eq(buckets[0].control.max[timeFieldName], times[2]);

MongoRunner.stopMongod(conn);
})();
