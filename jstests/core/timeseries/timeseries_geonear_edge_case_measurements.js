/**
 * Test the behavior of $geoNear minDistance/maxDistance on time-series measurements.
 *
 * @tags: [
 *   # Time series geo functionality requires pipeline optimization
 *   requires_pipeline_optimization,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/geo.js");
load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesMetricIndexes feature flag is not enabled.");
    return;
}
Random.setRandomSeed(7813223789272959000);

// Value is taken from geoconstants.h.
const earthRadiusMeters = (6378.1 * 1000);
const earthCircumferenceMeters = earthRadiusMeters * Math.PI * 2;

// https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Number/EPSILON
const epsilon = Math.pow(2, -52);

function wrap(input, min, max) {
    const span = max - min;
    if (input > max)
        input -= span;
    if (input < min)
        input += span;
    assert.lte(input, max);
    assert.gte(input, min);
    return input;
}

function clamp(input, min, max) {
    return Math.max(min, Math.min(input, max));
}

const queryPoint = randomLongLat();
jsTestLog("Query point: " + queryPoint);

const clusterCenter = randomLongLat();
jsTestLog("Cluster center: " + clusterCenter);

// Determine how far away the center of the cluster is from the query point.
let sphereDist, flatDist;
{
    const temp = db.getCollection(jsTestName() + '_temp');
    temp.drop();
    assert.commandWorked(temp.insert({loc: clusterCenter}));
    assert.commandWorked(temp.createIndex({loc: '2dsphere'}));
    assert.commandWorked(temp.createIndex({loc: '2d'}));

    sphereDist = temp.aggregate({
                         $geoNear: {
                             near: {type: "Point", coordinates: queryPoint},
                             key: 'loc',
                             distanceField: 'dist',
                         }
                     })
                     .toArray()[0]
                     .dist;

    flatDist = temp.aggregate({
                       $geoNear: {
                           near: queryPoint,
                           key: 'loc',
                           distanceField: 'dist',
                       }
                   })
                   .toArray()[0]
                   .dist;

    temp.drop();
}
jsTestLog("Center point has a SPHERE distance of " + sphereDist);
jsTestLog("Center point has a FLAT distance of " + flatDist);

// Create a cluster of points extremely close together.
const clusterPoints = [];
// We want the cluster of points to be close together, but not so close that their computed distance
// from the query point is the same. The smallest possible difference in distance is roughly
// distance*epsilon. We nudge each point by more than that to ensure they end up with different
// distances from the query point.
const sphereDistDegrees = sphereDist * 360 / earthCircumferenceMeters;
const deltas = [-2 * sphereDistDegrees * epsilon, 0, 2 * sphereDistDegrees * epsilon];
for (const dx of deltas) {
    for (const dy of deltas) {
        const x = wrap(clusterCenter[0] + dx, -180, +180);
        const y = clamp(clusterCenter[1] + dy, -90, +90);
        clusterPoints.push([x, y]);
    }
}
jsTestLog("Generated the following points:");
printjson(clusterPoints);

// Set up a normal and a time-series collection to compare results.
const coll = db.getCollection(jsTestName() + '_normal');
const tsColl = db.getCollection(jsTestName() + '_timeseries');
coll.drop();
tsColl.drop();
assert.commandWorked(coll.createIndex({loc: '2dsphere'}));
assert.commandWorked(coll.createIndex({loc: '2d'}));
assert.commandWorked(db.createCollection(tsColl.getName(), {timeseries: {timeField: 'time'}}));

const docs = clusterPoints.map(point => ({
                                   time: ISODate(),
                                   loc: point,
                               }));
assert.commandWorked(coll.insert(docs));
assert.commandWorked(tsColl.insert(docs));

// Compare time-series vs non-time-series.
for (const minOrMax of ['maxDistance', 'minDistance']) {
    const pipeline = [
        {
            $geoNear: {
                near: {type: "Point", coordinates: queryPoint},
                key: 'loc',
                distanceField: 'dist',
                [minOrMax]: sphereDist,
            }
        },
        {$sort: {'loc.0': 1, 'loc.1': 1}},
        {
            $project: {
                _id: 0,
                loc: "$loc",
                dist: "$dist",
            }
        }
    ];
    const result = coll.aggregate(pipeline).toArray();
    // In most cases we expect the query to find some but not all the points.
    // In rare cases (at a pole) the points could be clamped together.
    jsTestLog("Spherical " + minOrMax + " query included " + result.length + " out of " +
              docs.length + " points.");

    // Make sure the time-series results match.
    const tsResult = tsColl.aggregate(pipeline).toArray();
    assert.sameMembers(result, tsResult);
}

// Test the same thing for flat queries.
for (const minOrMax of ['maxDistance', 'minDistance']) {
    const pipeline = [
        {
            $geoNear: {
                near: queryPoint,
                key: 'loc',
                distanceField: 'dist',
                [minOrMax]: flatDist,
            }
        },
        {$sort: {'loc.0': 1, 'loc.1': 1}},
        {
            $project: {
                _id: 0,
                loc: "$loc",
                dist: "$dist",
            }
        }
    ];
    const result = coll.aggregate(pipeline).toArray();
    // In most cases we expect the query to find some but not all the points.
    // In rare cases (at a pole) the points could be clamped together.
    jsTestLog("Flat " + minOrMax + " query included " + result.length + " out of " + docs.length +
              " points.");

    // Make sure the time-series results match.
    const tsResult = tsColl.aggregate(pipeline).toArray();
    assert.sameMembers(result, tsResult);
}
})();
