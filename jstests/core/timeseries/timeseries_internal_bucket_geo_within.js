/**
 * Tests that $_internalBucketGeoWithin is correctly created and used to push down $geoWithin past
 * $_internalUnpackBucket when used on a non-metadata field on a time-series collection.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   requires_fcv_51,
 *   requires_pipeline_optimization,
 *   requires_timeseries,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const coll = db.timeseries_internal_bucket_geo_within;
coll.drop();

assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));

const pipeline = [{
    $match: {
        loc: {
            $geoWithin:
                {$geometry: {type: "Polygon", coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}}
        }
    }
}];

// Test that $_internalBucketGeoWithin is successfully created and used to optimize $geoWithin.
const explain = coll.explain().aggregate(pipeline);
const collScanStages = getAggPlanStages(explain, "COLLSCAN");
for (let collScanStage of collScanStages) {
    assert.docEq({
        "$_internalBucketGeoWithin": {
            "withinRegion": {
                "$geometry": {"type": "Polygon", "coordinates": [[[0, 0], [3, 6], [6, 1], [0, 0]]]}
            },
            "field": "loc"
        }
    },
                 collScanStage.filter,
                 collScanStages);
}

// Test that $geoWithin still gives the correct result, when the events are in the same bucket.
assert.commandWorked(coll.insert([
    {a: 1, loc: {type: "Point", coordinates: [0, 1]}, time: ISODate(), meta: {sensorId: 100}},
    {a: 2, loc: {type: "Point", coordinates: [2, 7]}, time: ISODate(), meta: {sensorId: 100}},
    {a: 3, loc: {type: "Point", coordinates: [2, 1]}, time: ISODate(), meta: {sensorId: 100}}
]));
let results = coll.aggregate(pipeline).toArray();
// Only one document should match the given query.
assert.eq(results.length, 1, results);

// Test that $geoWithin still gives the correct result, when the events are in different buckets.
assert.commandWorked(coll.insert([
    {a: 1, loc: {type: "Point", coordinates: [0, 1]}, time: ISODate(), meta: {sensorId: 101}},
    {a: 2, loc: {type: "Point", coordinates: [2, 7]}, time: ISODate(), meta: {sensorId: 102}},
    {a: 3, loc: {type: "Point", coordinates: [2, 1]}, time: ISODate(), meta: {sensorId: 103}}
]));
results = coll.aggregate(pipeline).toArray();
// Two documents should match the given query.
assert.eq(results.length, 2, results);

// Test a scenario where the control fields do not properly summarize the events:
// 'a' is a mixture of objects and scalars.
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
assert.commandWorked(coll.insert([
    {time: ISODate(), a: 5},
    {time: ISODate(), a: {b: {type: "Point", coordinates: [0, 0]}}},
    {time: ISODate(), a: ISODate('2020-01-01')},
]));
results = coll.aggregate([
                  {$match: {'a.b': {$geoWithin: {$centerSphere: [[0, 0], 100]}}}},
              ])
              .toArray();
assert.eq(results.length, 1, results);
assert.docEq(results[0].a, {b: {type: "Point", coordinates: [0, 0]}});

// Test a scenario where $geoWithin does implicit array traversal.
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
assert.commandWorked(coll.insert([
    {
        time: ISODate(),
        a: [
            12345,
            {type: "Point", coordinates: [180, 0]},
            {"1": {type: "Point", coordinates: [0, 0]}},
        ]
    },
]));
results = coll.aggregate([
                  // The bucket-level predicate does not do any implicit array traversal, so 'a.1'
                  // refers to the point [180, 0].  (So it rejects the bucket.)  But $geoWithin does
                  // do implicit array traversal, so 'a.1' refers to the "1" field of any element of
                  // 'a'.  (So it should include the event.)
                  {$match: {'a.1': {$geoWithin: {$centerSphere: [[0, 0], 1]}}}},
              ])
              .toArray();
assert.eq(results.length, 1, results);
assert.docEq(results[0].a, [
    12345,
    {type: "Point", coordinates: [180, 0]},
    {"1": {type: "Point", coordinates: [0, 0]}},
]);
}());