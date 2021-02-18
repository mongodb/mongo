/**
 * Tests basic index creation and operations on a time-series bucket collection.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 *     sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/analyze_plan.js");  // For 'planHasStage' helper.

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection('ts');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

const timeFieldName = 'time';
assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

assert.commandWorked(bucketsColl.createIndex({"control.min.time": 1}));

const t = new Date();
const doc = {
    _id: 0,
    [timeFieldName]: t,
    x: 0
};
assert.commandWorked(coll.insert(doc), 'failed to insert doc: ' + tojson(doc));

assert.commandWorked(bucketsColl.createIndex({"control.max.time": 1}));

let buckets = bucketsColl.find().toArray();
assert.eq(buckets.length, 1, 'Expected one bucket but found ' + tojson(buckets));
const bucketId = buckets[0]._id;
const minTime = buckets[0].control.min.time;
const maxTime = buckets[0].control.min.time;

assert.docEq(buckets, bucketsColl.find({_id: bucketId}).toArray());
let explain = bucketsColl.find({_id: bucketId}).explain();
assert(planHasStage(testDB, explain, "COLLSCAN"), explain);

assert.docEq(buckets, bucketsColl.find({"control.min.time": minTime}).toArray());
explain = bucketsColl.find({"control.min.time": minTime}).explain();
assert(planHasStage(testDB, explain, "IXSCAN"), explain);

assert.docEq(buckets, bucketsColl.find({"control.max.time": maxTime}).toArray());
explain = bucketsColl.find({"control.max.time": minTime}).explain();
assert(planHasStage(testDB, explain, "IXSCAN"), explain);

let res = assert.commandWorked(bucketsColl.validate());
assert(res.valid, res);

assert.commandWorked(bucketsColl.remove({_id: bucketId}));
assert.docEq([], bucketsColl.find().toArray());
})();
