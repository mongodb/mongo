// @tags: [
//   requires_getmore,
// ]

//
// Tests the error handling of spherical queries
// along with multi-location documents.
// This is necessary since the error handling must manage
// multiple documents, and so requires simultaneous testing.
//

import {deg2rad, rad2deg} from "jstests/libs/query/geo_math.js";

function computexscandist(y, maxDistDegrees) {
    return (
        maxDistDegrees /
        Math.min(
            Math.cos(deg2rad(Math.min(89.0, y + maxDistDegrees))),
            Math.cos(deg2rad(Math.max(-89.0, y - maxDistDegrees))),
        )
    );
}

function pointIsOK(startPoint, radius) {
    let yscandist = rad2deg(radius) + 0.01;
    let xscandist = computexscandist(startPoint[1], yscandist);
    return (
        startPoint[0] + xscandist < 180 &&
        startPoint[0] - xscandist > -180 &&
        startPoint[1] + yscandist < 90 &&
        startPoint[1] - yscandist > -90
    );
}

let numTests = 30;

for (let test = 0; test < numTests; test++) {
    Random.srand(1337 + test);

    let radius = 5000 * Random.rand(); // km
    radius = radius / 6378.1; // radians; earth radius from geoconstants.h
    let numDocs = Math.floor(400 * Random.rand());
    // TODO: Wrapping uses the error value to figure out what would overlap...
    let bits = Math.floor(5 + Random.rand() * 28);
    let maxPointsPerDoc = 50;

    let t = db.sphere;

    let randomPoint = function () {
        return [Random.rand() * 360 - 180, Random.rand() * 180 - 90];
    };

    // Get a start point that doesn't require wrapping
    // TODO: Are we a bit too aggressive with wrapping issues?
    var startPoint;
    let ex = null;
    do {
        t.drop();
        startPoint = randomPoint();
        t.createIndex({loc: "2d"}, {bits: bits});
    } while (!pointIsOK(startPoint, radius));

    let pointsIn = 0;
    let pointsOut = 0;
    let docsIn = 0;
    let docsOut = 0;
    let totalPoints = 0;

    let bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        let numPoints = Math.floor(Random.rand() * maxPointsPerDoc + 1);
        let docIn = false;
        let multiPoint = [];

        totalPoints += numPoints;

        for (let p = 0; p < numPoints; p++) {
            let point = randomPoint();
            multiPoint.push(point);

            if (Geo.sphereDistance(startPoint, point) <= radius) {
                pointsIn++;
                docIn = true;
            } else {
                pointsOut++;
            }
        }

        bulk.insert({loc: multiPoint});

        if (docIn) docsIn++;
        else docsOut++;
    }

    printjson({
        test: test,
        radius: radius,
        bits: bits,
        numDocs: numDocs,
        pointsIn: pointsIn,
        docsIn: docsIn,
        pointsOut: pointsOut,
        docsOut: docsOut,
    });

    assert.commandWorked(bulk.execute());
    assert.eq(docsIn + docsOut, numDocs);
    assert.eq(pointsIn + pointsOut, totalPoints);

    // $centerSphere
    assert.eq(docsIn, t.find({loc: {$within: {$centerSphere: [startPoint, radius]}}}).count());

    // $nearSphere
    let cursor = t.find({loc: {$nearSphere: startPoint, $maxDistance: radius}});
    let results = cursor.limit(2 * pointsIn).toArray();

    assert.eq(docsIn, results.length);

    var distance = 0;
    for (var i = 0; i < results.length; i++) {
        let minNewDistance = radius + 1;
        for (var j = 0; j < results[i].loc.length; j++) {
            var newDistance = Geo.sphereDistance(startPoint, results[i].loc[j]);
            if (newDistance < minNewDistance && newDistance >= distance) {
                minNewDistance = newDistance;
            }
        }

        // print( "Dist from : " + results[i].loc[j] + " to " + startPoint + " is "
        // + minNewDistance + " vs " + radius )

        assert.lte(minNewDistance, radius);
        assert.gte(minNewDistance, distance);
        distance = minNewDistance;
    }

    // Test $geoNear.
    results = t
        .aggregate({
            $geoNear: {
                near: startPoint,
                distanceField: "dis",
                maxDistance: radius,
                spherical: true,
            },
        })
        .toArray();
    assert.eq(docsIn, results.length, tojson(results));

    var distance = 0;
    for (var i = 0; i < results.length; i++) {
        let retDistance = results[i].dis;

        let distInObj = false;
        for (var j = 0; j < results[i].loc.length && distInObj == false; j++) {
            var newDistance = Geo.sphereDistance(startPoint, results[i].loc[j]);
            distInObj = newDistance >= retDistance - 0.0001 && newDistance <= retDistance + 0.0001;
        }

        assert(distInObj);
        assert.lte(retDistance, radius);
        assert.gte(retDistance, distance);
        distance = retDistance;
    }
}
