/**
 * Test the behavior of $geoNear queries on randomly chosen time-series measurements.
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
Random.setRandomSeed();

// Value is taken from geoconstants.h.
const earthRadiusMeters = (6378.1 * 1000);
const earthCircumferenceMeters = earthRadiusMeters * Math.PI * 2;

const numDocs = 100;
let docs = [];
while (docs.length < numDocs) {
    const [long, lat] = randomLongLat();

    // Each document will be used as the center of a query, so also include a randomly chosen
    // minDistance and maxDistance.
    const lenMeters = Random.rand() * earthCircumferenceMeters;
    const minDistanceMeters = Random.rand() * (earthCircumferenceMeters - lenMeters);
    const maxDistanceMeters = minDistanceMeters + lenMeters;

    // When interpreted as flat 2d coordinates, the points all lie in a rectangle of width 2pi
    // and height pi.  Choose a random min/max to use in flat queries.
    const minDistanceFlat = Random.rand() * (Math.PI / 2);
    const maxDistanceFlat = Random.rand() * (Math.PI / 2) + minDistanceFlat;

    docs.push({
        _id: docs.length,
        time: ISODate(),
        loc: [long, lat],
        minDistanceMeters,
        maxDistanceMeters,
        minDistanceFlat,
        maxDistanceFlat,
    });
}
jsTestLog("Generated the following documents:");
printjson(docs);

const coll = db.getCollection(jsTestName() + '_normal');
const tsColl = db.getCollection(jsTestName() + '_timeseries');
coll.drop();
tsColl.drop();
assert.commandWorked(coll.createIndex({loc: '2dsphere'}));
assert.commandWorked(coll.createIndex({loc: '2d'}));
assert.commandWorked(db.createCollection(tsColl.getName(), {timeseries: {timeField: 'time'}}));
assert.commandWorked(coll.insert(docs));
assert.commandWorked(tsColl.insert(docs));

function assertSortedAscending(numbers) {
    for (let i = 1; i < numbers.length; ++i) {
        assert.lte(numbers[i - 1], numbers[i], 'Found two descending elements at position ' + i);
    }
}

// Run a query centered on each point we inserted.
// - The result-set should be the same for both collections.
// - Each result should be sorted by 'dist'.
// - The two result-sets may disagree on the order of ties, so we don't compare the order directly.
for (const doc of docs) {
    print('Testing sphere query centered at ' + tojson(doc));
    const {minDistanceMeters, maxDistanceMeters, loc: [long, lat]} = doc;
    const pipeline = [{
        $geoNear: {
            near: {type: "Point", coordinates: [long, lat]},
            key: 'loc',
            distanceField: 'dist',
            spherical: true,
            minDistance: minDistanceMeters,
            maxDistance: maxDistanceMeters,
        }
    }];
    const result = coll.aggregate(pipeline).toArray();
    const tsResult = tsColl.aggregate(pipeline).toArray();
    assert.sameMembers(result, tsResult);
    assertSortedAscending(result.map(d => d.dist));
    assertSortedAscending(tsResult.map(d => d.dist));
    print('Got ' + result.length + ' results');
}

// Do the same thing, but interpreting the points as lying in a plane.
for (const doc of docs) {
    print('Testing flat query centered at ' + tojson(doc));
    const {minDistanceFlat, maxDistanceFlat, loc: [long, lat]} = doc;
    const pipeline = [{
        $geoNear: {
            near: [long, lat],
            key: 'loc',
            distanceField: 'dist',
            spherical: false,
            minDistance: minDistanceFlat,
            maxDistance: maxDistanceFlat,
        }
    }];
    const result = coll.aggregate(pipeline).toArray();
    const tsResult = tsColl.aggregate(pipeline).toArray();
    assert.sameMembers(result, tsResult);
    assertSortedAscending(result.map(d => d.dist));
    assertSortedAscending(tsResult.map(d => d.dist));
    print('Got ' + result.length + ' results');
}
})();
