//
// Tests the error handling of spherical queries
// along with multi-location documents.
// This is necessary since the error handling must manage
// multiple documents, and so requires simultaneous testing.
//

function deg2rad(arg) {
    return arg * Math.PI / 180.0;
}
function rad2deg(arg) {
    return arg * 180.0 / Math.PI;
}

function computexscandist(y, maxDistDegrees) {
    return maxDistDegrees /
        Math.min(Math.cos(deg2rad(Math.min(89.0, y + maxDistDegrees))),
                 Math.cos(deg2rad(Math.max(-89.0, y - maxDistDegrees))));
}

function pointIsOK(startPoint, radius) {
    yscandist = rad2deg(radius) + 0.01;
    xscandist = computexscandist(startPoint[1], yscandist);
    return (startPoint[0] + xscandist < 180) && (startPoint[0] - xscandist > -180) &&
        (startPoint[1] + yscandist < 90) && (startPoint[1] - yscandist > -90);
}

var numTests = 30;

for (var test = 0; test < numTests; test++) {
    Random.srand(1337 + test);

    var radius = 5000 * Random.rand();  // km
    radius = radius / 6378.1;           // radians; earth radius from geoconstants.h
    var numDocs = Math.floor(400 * Random.rand());
    // TODO: Wrapping uses the error value to figure out what would overlap...
    var bits = Math.floor(5 + Random.rand() * 28);
    var maxPointsPerDoc = 50;

    var t = db.sphere;

    var randomPoint = function() {
        return [Random.rand() * 360 - 180, Random.rand() * 180 - 90];
    };

    // Get a start point that doesn't require wrapping
    // TODO: Are we a bit too aggressive with wrapping issues?
    var startPoint;
    var ex = null;
    do {
        t.drop();
        startPoint = randomPoint();
        t.ensureIndex({loc: "2d"}, {bits: bits});
    } while (!pointIsOK(startPoint, radius));

    var pointsIn = 0;
    var pointsOut = 0;
    var docsIn = 0;
    var docsOut = 0;
    var totalPoints = 0;

    var bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        var numPoints = Math.floor(Random.rand() * maxPointsPerDoc + 1);
        var docIn = false;
        var multiPoint = [];

        totalPoints += numPoints;

        for (var p = 0; p < numPoints; p++) {
            var point = randomPoint();
            multiPoint.push(point);

            if (Geo.sphereDistance(startPoint, point) <= radius) {
                pointsIn++;
                docIn = true;
            } else {
                pointsOut++;
            }
        }

        bulk.insert({loc: multiPoint});

        if (docIn)
            docsIn++;
        else
            docsOut++;
    }

    printjson({
        test: test,
        radius: radius,
        bits: bits,
        numDocs: numDocs,
        pointsIn: pointsIn,
        docsIn: docsIn,
        pointsOut: pointsOut,
        docsOut: docsOut
    });

    assert.writeOK(bulk.execute());
    assert.eq(docsIn + docsOut, numDocs);
    assert.eq(pointsIn + pointsOut, totalPoints);

    // $centerSphere
    assert.eq(docsIn, t.find({loc: {$within: {$centerSphere: [startPoint, radius]}}}).count());

    // $nearSphere
    var cursor = t.find({loc: {$nearSphere: startPoint, $maxDistance: radius}});
    var results = cursor.limit(2 * pointsIn).toArray();

    assert.eq(docsIn, results.length);

    var distance = 0;
    for (var i = 0; i < results.length; i++) {
        var minNewDistance = radius + 1;
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

    // geoNear
    results = db.runCommand({
                    geoNear: "sphere",
                    near: startPoint,
                    maxDistance: radius,
                    num: 2 * pointsIn,
                    spherical: true
                }).results;

    /*
    printjson( results );

    for ( var j = 0; j < results[0].obj.loc.length; j++ ) {
        var newDistance = Geo.sphereDistance( startPoint, results[0].obj.loc[j] )
        if( newDistance <= radius ) print( results[0].obj.loc[j] + " : " + newDistance )
    }
    */

    assert.eq(docsIn, results.length);

    var distance = 0;
    for (var i = 0; i < results.length; i++) {
        var retDistance = results[i].dis;

        // print( "Dist from : " + results[i].loc + " to " + startPoint + " is "
        // + retDistance + " vs " + radius )

        var distInObj = false;
        for (var j = 0; j < results[i].obj.loc.length && distInObj == false; j++) {
            var newDistance = Geo.sphereDistance(startPoint, results[i].obj.loc[j]);
            distInObj =
                (newDistance >= retDistance - 0.0001 && newDistance <= retDistance + 0.0001);
        }

        assert(distInObj);
        assert.lte(retDistance, radius);
        assert.gte(retDistance, distance);
        distance = retDistance;
    }
}
