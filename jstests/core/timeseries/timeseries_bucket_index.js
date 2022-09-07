/**
 * Tests basic index creation and operations on a time-series bucket collection.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/analyze_plan.js");  // For 'planHasStage' helper.

TimeseriesTest.run((insert) => {
    const coll = db.timeseries_bucket_index;
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

    const timeFieldName = 'time';

    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    assert.commandWorked(bucketsColl.createIndex({"control.min.time": 1}));

    const t = new Date();
    const doc = {_id: 0, [timeFieldName]: t, x: 0};
    assert.commandWorked(insert(coll, doc), 'failed to insert doc: ' + tojson(doc));

    assert.commandWorked(bucketsColl.createIndex({"control.max.time": 1}));

    let buckets = bucketsColl.find().toArray();
    assert.eq(buckets.length, 1, 'Expected one bucket but found ' + tojson(buckets));
    const bucketId = buckets[0]._id;
    const minTime = buckets[0].control.min.time;
    const maxTime = buckets[0].control.max.time;

    assert.docEq(buckets, bucketsColl.find({_id: bucketId}).toArray());
    let explain = bucketsColl.find({_id: bucketId}).explain();
    assert(planHasStage(db, explain, "CLUSTERED_IXSCAN"), explain);

    assert.docEq(buckets, bucketsColl.find({"control.max.time": maxTime}).toArray());
    explain = bucketsColl.find({"control.max.time": minTime}).explain();
    assert(planHasStage(db, explain, "IXSCAN"), explain);

    let res = assert.commandWorked(bucketsColl.validate());
    assert(res.valid, res);

    assert.commandWorked(bucketsColl.remove({_id: bucketId}));
    assert.docEq([], bucketsColl.find().toArray());
});
})();
