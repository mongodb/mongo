/**
 * Tests that $_internalBucketGeoWithin is correctly created and used to push down $geoWithin and
 * $geoIntersects past $_internalUnpackBucket when used on a non-metadata field on a time-series
 * collection.
 *
 * @tags: [
 *   requires_fcv_62,
 *   requires_pipeline_optimization,
 *   requires_timeseries,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const coll = db.timeseries_internal_bucket_geo_within;
coll.drop();

assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

let pipeline = [{
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
    let expectedPredicate = {
        "$_internalBucketGeoWithin": {
            "withinRegion": {
                "$geometry": {"type": "Polygon", "coordinates": [[[0, 0], [3, 6], [6, 1], [0, 0]]]}
            },
            "field": "loc"
        }
    };
    // TODO SERVER-60373 Fix duplicate predicates for sharded time-series collection
    if (FixtureHelpers.isSharded(bucketsColl)) {
        expectedPredicate = {$and: [expectedPredicate, expectedPredicate]};
    }
    assert.docEq(expectedPredicate, collScanStage.filter, collScanStages);
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

pipeline = [{
    $match: {
        loc: {
            $geoIntersects:
                {$geometry: {type: "Polygon", coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}}
        }
    }
}];

// Test that $geoIntersects and $geoWithin are equivalent for points. Each document is in a
// different bucket, so if $_internalBucketGeoWithin incorrectly filters a bucket out, we should
// know.
const now = ISODate();
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
assert.commandWorked(coll.insert([
    // Point on a polygon vertex
    {_id: 0, a: 1, loc: {type: "Point", coordinates: [0, 0]}, time: now, meta: {sensorId: 101}},
    // Point on a polygon line
    {_id: 1, a: 2, loc: {type: "Point", coordinates: [1, 2]}, time: now, meta: {sensorId: 102}},
    // Point inside
    {_id: 2, a: 3, loc: {type: "Point", coordinates: [2, 1]}, time: now, meta: {sensorId: 103}},
    // Point outside
    {_id: 3, a: 4, loc: {type: "Point", coordinates: [-1, -2]}, time: now, meta: {sensorId: 104}}
]));
results = coll.aggregate(pipeline).toArray();
assert.sameMembers(results, [
    {_id: 0, a: 1, loc: {type: "Point", coordinates: [0, 0]}, time: now, meta: {sensorId: 101}},
    {_id: 1, a: 2, loc: {type: "Point", coordinates: [1, 2]}, time: now, meta: {sensorId: 102}},
    {_id: 2, a: 3, loc: {type: "Point", coordinates: [2, 1]}, time: now, meta: {sensorId: 103}},
]);

// Test if a Point with an unexpected field still matches
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
assert.commandWorked(coll.insert([
    {
        _id: 0,
        a: 1,
        loc: {type: "Point", coordinates: [0, 1], unexpected_field: 1},
        time: now,
        meta: {sensorId: 101}
    },
    {
        _id: 1,
        a: 2,
        loc: {type: "Point", coordinates: [2, 7], unexpected_field: 1},
        time: now,
        meta: {sensorId: 102}
    },
    {
        _id: 2,
        a: 3,
        loc: {type: "Point", coordinates: [2, 1], unexpected_field: 1},
        time: now,
        meta: {sensorId: 103}
    }
]));
results = coll.aggregate(pipeline).toArray();
assert.sameMembers(results, [{
                       _id: 2,
                       a: 3,
                       loc: {type: "Point", coordinates: [2, 1], unexpected_field: 1},
                       time: now,
                       meta: {sensorId: 103}
                   }]);

// Test that we match the bucket if the types are not Point or control min and max are different
// types such as date and array
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
assert.commandWorked(coll.insert([
    {_id: 0, a: 1, loc: now, time: now, meta: {sensorId: 101}},
    {_id: 1, a: 1, loc: [5, 6, 7], time: now, meta: {sensorId: 101}},
    {_id: 2, a: 3, loc: now, time: now, meta: {sensorId: 103}},
    {_id: 3, a: 3, loc: [5, 6, 7], time: now, meta: {sensorId: 103}},
    {_id: 4, a: 3, loc: {type: "Point", coordinates: [2, 1]}, time: now, meta: {sensorId: 103}}
]));
results = coll.aggregate(pipeline).toArray();
assert.sameMembers(
    results,
    [{_id: 4, a: 3, loc: {type: "Point", coordinates: [2, 1]}, time: now, meta: {sensorId: 103}}]);

// Try to make $_internalBucketGeoWithin fail with null/undefined fields within a bucket
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
assert.commandWorked(coll.insert([
    {_id: 0, a: 3, loc: undefined, time: now, meta: {sensorId: 103}},
    {_id: 1, a: 3, loc: null, time: now, meta: {sensorId: 103}},
    {_id: 2, a: 3, loc: {type: "Point", coordinates: [2, 1]}, time: now, meta: {sensorId: 103}}
]));
results = coll.aggregate(pipeline).toArray();
assert.sameMembers(
    results,
    [{_id: 2, a: 3, loc: {type: "Point", coordinates: [2, 1]}, time: now, meta: {sensorId: 103}}]);

// Query on a field within object, so that we can test for correct behavior if the object containing
// the field is null/undefined/missing or other.
pipeline = [{
    $match: {
        "x.y": {
            $geoIntersects:
                {$geometry: {type: "Polygon", coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}}
        }
    }
}];

// Missing y field within x object should still allow us to match the bucket and find points within
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
assert.commandWorked(coll.insert([
    {_id: 0, a: 1, x: {}, time: now, meta: {sensorId: 100}},
    {_id: 1, a: 2, x: {y: {type: "Point", coordinates: [2, 7]}}, time: now, meta: {sensorId: 100}},
    {_id: 2, a: 3, x: {y: {type: "Point", coordinates: [2, 1]}}, time: now, meta: {sensorId: 100}}
]));
results = coll.aggregate(pipeline).toArray();
assert.sameMembers(results, [
    {_id: 2, a: 3, x: {y: {type: "Point", coordinates: [2, 1]}}, time: now, meta: {sensorId: 100}}
]);

// x can be undefined/null/empty, but we should still match the bucket
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
assert.commandWorked(coll.insert([
    {_id: 0, a: 1, x: undefined, time: now, meta: {sensorId: 100}},
    {_id: 1, a: 2, x: null, time: now, meta: {sensorId: 100}},
    {_id: 2, a: 2, x: {}, time: now, meta: {sensorId: 100}},
    {_id: 3, a: 3, x: {y: {type: "Point", coordinates: [2, 1]}}, time: now, meta: {sensorId: 100}},
    {_id: 4, a: 3, x: {y: {type: "Point", coordinates: [3, 1]}}, time: now, meta: {sensorId: 100}}
]));
results = coll.aggregate(pipeline).toArray();
assert.sameMembers(results, [
    {_id: 3, a: 3, x: {y: {type: "Point", coordinates: [2, 1]}}, time: now, meta: {sensorId: 100}},
    {_id: 4, a: 3, x: {y: {type: "Point", coordinates: [3, 1]}}, time: now, meta: {sensorId: 100}}
]);

// x can be some other object such as a date or an array, again we still match
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
assert.commandWorked(coll.insert([
    {_id: 0, a: 1, x: now, time: now, meta: {sensorId: 100}},
    {_id: 1, a: 2, x: [1, 2, 3], time: now, meta: {sensorId: 100}},
    {_id: 2, a: 3, x: {y: undefined}, time: now, meta: {sensorId: 100}},
    {_id: 3, a: 4, x: {y: {type: "Point", coordinates: [2, 1]}}, time: now, meta: {sensorId: 100}}
]));
results = coll.aggregate(pipeline).toArray();
assert.sameMembers(results, [
    {_id: 3, a: 4, x: {y: {type: "Point", coordinates: [2, 1]}}, time: now, meta: {sensorId: 100}}
]);

// Test some parse errors.
{
    coll.drop();

    pipeline = [{$match: {$_internalBucketGeoWithin: {}}}];
    let err = assert.throws(() => coll.explain().aggregate(pipeline));
    assert.eq(err.code, ErrorCodes.FailedToParse, err);

    pipeline = [{
        $match: {
            $_internalBucketGeoWithin: {
                withinRegion: 7,
                field: 'loc',
            }
        }
    }];
    err = assert.throws(() => coll.explain().aggregate(pipeline));
    assert.eq(err.code, ErrorCodes.TypeMismatch, err);

    pipeline = [{
        $match: {
            $_internalBucketGeoWithin: {
                withinRegion: {},
                field: 'loc',
            }
        }
    }];
    err = assert.throws(() => coll.explain().aggregate(pipeline));
    assert.eq(err.code, ErrorCodes.BadValue, err);

    // $geoWithin doesn't support multiple shapes.
    pipeline = [{$match: {loc: {$geoWithin: {$centerSphere: [[0, 80], 1], $center: [[0, 0], 5]}}}}];
    err = assert.throws(() => coll.explain().aggregate(pipeline));
    assert.eq(err.code, ErrorCodes.BadValue, err);
}
}());
