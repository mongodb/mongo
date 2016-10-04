//
// Integration test of the geo code
//
// Basically, this tests adds a random number of docs with a random number of points,
// given a 2d environment of random precision which is either randomly earth-like or of
// random bounds, and indexes these points after a random amount of points have been added
// with a random number of additional fields which correspond to whether the documents are
// in randomly generated circular, spherical, box, and box-polygon shapes (and exact),
// queried randomly from a set of query types.  Each point is randomly either and object
// or array, and all points and document data fields are nested randomly in arrays (or not).
//
// We approximate the user here as a random function :-)
//
// These random point fields can then be tested against all types of geo queries using these random
// shapes.
//
// Tests can be easily reproduced by getting the test number from the output directly before a
// test fails, and hard-wiring that as the test number.
//

load("jstests/libs/slow_weekly_util.js");
testServer = new SlowWeeklyMongod("geo_full");
db = testServer.getDB("test");

var randEnvironment = function() {

    // Normal earth environment
    if (Random.rand() < 0.5) {
        return {
            max: 180,
            min: -180,
            bits: Math.floor(Random.rand() * 32) + 1,
            earth: true,
            bucketSize: 360 / (4 * 1024 * 1024 * 1024)
        };
    }

    var scales = [0.0001, 0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000, 100000];
    var scale = scales[Math.floor(Random.rand() * scales.length)];
    var offset = Random.rand() * scale;

    var max = Random.rand() * scale + offset;
    var min = -Random.rand() * scale + offset;
    var bits = Math.floor(Random.rand() * 32) + 1;
    var bits = Math.floor(Random.rand() * 32) + 1;
    var range = max - min;
    var bucketSize = range / (4 * 1024 * 1024 * 1024);

    return {max: max, min: min, bits: bits, earth: false, bucketSize: bucketSize};
};

var randPoint = function(env, query) {

    if (query && Random.rand() > 0.5)
        return query.exact;

    if (env.earth)
        return [Random.rand() * 360 - 180, Random.rand() * 180 - 90];

    var range = env.max - env.min;
    return [Random.rand() * range + env.min, Random.rand() * range + env.min];
};

var randLocType = function(loc, wrapIn) {
    return randLocTypes([loc], wrapIn)[0];
};

var randLocTypes = function(locs, wrapIn) {

    var rLocs = [];

    for (var i = 0; i < locs.length; i++) {
        rLocs.push(locs[i]);
    }

    if (wrapIn) {
        var wrappedLocs = [];
        for (var i = 0; i < rLocs.length; i++) {
            var wrapper = {};
            wrapper[wrapIn] = rLocs[i];
            wrappedLocs.push(wrapper);
        }

        return wrappedLocs;
    }

    return rLocs;
};

var randDataType = function() {

    var scales = [1, 10, 100, 1000, 10000];
    var docScale = scales[Math.floor(Random.rand() * scales.length)];
    var locScale = scales[Math.floor(Random.rand() * scales.length)];

    var numDocs = 40000;
    var maxLocs = 40000;
    // Make sure we don't blow past our test resources
    while (numDocs * maxLocs > 40000) {
        numDocs = Math.floor(Random.rand() * docScale) + 1;
        maxLocs = Math.floor(Random.rand() * locScale) + 1;
    }

    return {numDocs: numDocs, maxLocs: maxLocs};
};

function deg2rad(arg) {
    return arg * Math.PI / 180.0;
}
function rad2deg(arg) {
    return arg * 180.0 / Math.PI;
}

function computexscandist(latDegrees, maxDistDegrees) {
    // See s2cap.cc
    //
    // Compute the range of longitudes covered by the cap.  We use the law
    // of sines for spherical triangles.  Consider the triangle ABC where
    // A is the north pole, B is the center of the cap, and C is the point
    // of tangency between the cap boundary and a line of longitude.  Then
    // C is a right angle, and letting a,b,c denote the sides opposite A,B,C,
    // we have sin(a)/sin(A) = sin(c)/sin(C), or sin(A) = sin(a)/sin(c).
    // Here "a" is the cap angle, and "c" is the colatitude (90 degrees
    // minus the latitude).  This formula also works for negative latitudes.
    //
    // Angle A is the difference of longitudes of B and C.
    var sin_c = Math.cos(deg2rad(latDegrees));
    var sin_a = Math.sin(deg2rad(maxDistDegrees));
    if (sin_a > sin_c) {
        // Double floating number error, return invalid distance
        return 180;
    }
    var angleA = Math.asin(sin_a / sin_c);
    return rad2deg(angleA);
}

function errorMarginForPoint(env) {
    if (!env.bits) {
        return 0.01;
    }
    var scalingFactor = Math.pow(2, env.bits);
    return ((env.max - env.min) / scalingFactor) * Math.sqrt(2);
}

function pointIsOK(startPoint, radius, env) {
    var error = errorMarginForPoint(env);
    var distDegrees = rad2deg(radius) + error;
    // TODO SERVER-24440: Points close to the north and south poles may fail to be returned by
    // $nearSphere queries answered using a "2d" index. We have empirically found that points with
    // latitudes between 89 and 90 degrees are potentially affected by this issue, so we
    // additionally reject any coordinates with a latitude that falls within that range.
    if ((startPoint[1] + distDegrees > 89) || (startPoint[1] - distDegrees < -89)) {
        return false;
    }
    var xscandist = computexscandist(startPoint[1], distDegrees);
    return (startPoint[0] + xscandist < 180) && (startPoint[0] - xscandist > -180);
}

var randQuery = function(env) {
    var center = randPoint(env);

    var sphereRadius = -1;
    var sphereCenter = null;
    if (env.earth) {
        // Get a start point that doesn't require wrapping
        // TODO: Are we a bit too aggressive with wrapping issues?
        var i;
        for (i = 0; i < 5; i++) {
            sphereRadius = Random.rand() * 45 * Math.PI / 180;
            sphereCenter = randPoint(env);
            if (pointIsOK(sphereCenter, sphereRadius, env)) {
                break;
            }
        }
        if (i == 5)
            sphereRadius = -1;
    }

    var box = [randPoint(env), randPoint(env)];

    var boxPoly = [
        [box[0][0], box[0][1]],
        [box[0][0], box[1][1]],
        [box[1][0], box[1][1]],
        [box[1][0], box[0][1]]
    ];

    if (box[0][0] > box[1][0]) {
        var swap = box[0][0];
        box[0][0] = box[1][0];
        box[1][0] = swap;
    }

    if (box[0][1] > box[1][1]) {
        var swap = box[0][1];
        box[0][1] = box[1][1];
        box[1][1] = swap;
    }

    return {
        center: center,
        radius: box[1][0] - box[0][0],
        exact: randPoint(env),
        sphereCenter: sphereCenter,
        sphereRadius: sphereRadius,
        box: box,
        boxPoly: boxPoly
    };
};

var resultTypes = {
    "exact": function(loc) {
        return query.exact[0] == loc[0] && query.exact[1] == loc[1];
    },
    "center": function(loc) {
        return Geo.distance(query.center, loc) <= query.radius;
    },
    "box": function(loc) {
        return loc[0] >= query.box[0][0] && loc[0] <= query.box[1][0] &&
            loc[1] >= query.box[0][1] && loc[1] <= query.box[1][1];

    },
    "sphere": function(loc) {
        return (query.sphereRadius >= 0
                    ? (Geo.sphereDistance(query.sphereCenter, loc) <= query.sphereRadius)
                    : false);
    },
    "poly": function(loc) {
        return loc[0] >= query.box[0][0] && loc[0] <= query.box[1][0] &&
            loc[1] >= query.box[0][1] && loc[1] <= query.box[1][1];
    }
};

var queryResults = function(locs, query, results) {

    if (!results["center"]) {
        for (var type in resultTypes) {
            results[type] = {docsIn: 0, docsOut: 0, locsIn: 0, locsOut: 0};
        }
    }

    var indResults = {};
    for (var type in resultTypes) {
        indResults[type] = {docIn: false, locsIn: 0, locsOut: 0};
    }

    for (var type in resultTypes) {
        var docIn = false;
        for (var i = 0; i < locs.length; i++) {
            if (resultTypes[type](locs[i])) {
                results[type].locsIn++;
                indResults[type].locsIn++;
                indResults[type].docIn = true;
            } else {
                results[type].locsOut++;
                indResults[type].locsOut++;
            }
        }
        if (indResults[type].docIn)
            results[type].docsIn++;
        else
            results[type].docsOut++;
    }

    return indResults;
};

var randQueryAdditions = function(doc, indResults) {

    for (var type in resultTypes) {
        var choice = Random.rand();
        if (Random.rand() < 0.25)
            doc[type] = (indResults[type].docIn ? {docIn: "yes"} : {docIn: "no"});
        else if (Random.rand() < 0.5)
            doc[type] = (indResults[type].docIn ? {docIn: ["yes"]} : {docIn: ["no"]});
        else if (Random.rand() < 0.75)
            doc[type] = (indResults[type].docIn ? [{docIn: "yes"}] : [{docIn: "no"}]);
        else
            doc[type] = (indResults[type].docIn ? [{docIn: ["yes"]}] : [{docIn: ["no"]}]);
    }
};

var randIndexAdditions = function(indexDoc) {

    for (var type in resultTypes) {
        if (Random.rand() < 0.5)
            continue;

        var choice = Random.rand();
        if (Random.rand() < 0.5)
            indexDoc[type] = 1;
        else
            indexDoc[type + ".docIn"] = 1;
    }
};

var randYesQuery = function() {

    var choice = Math.floor(Random.rand() * 7);
    if (choice == 0)
        return {$ne: "no"};
    else if (choice == 1)
        return "yes";
    else if (choice == 2)
        return /^yes/;
    else if (choice == 3)
        return {$in: ["good", "yes", "ok"]};
    else if (choice == 4)
        return {$exists: true};
    else if (choice == 5)
        return {$nin: ["bad", "no", "not ok"]};
    else if (choice == 6)
        return {$not: /^no/};
};

var locArray = function(loc) {
    if (loc.x)
        return [loc.x, loc.y];
    if (!loc.length)
        return [loc[0], loc[1]];
    return loc;
};

var locsArray = function(locs) {
    if (locs.loc) {
        arr = [];
        for (var i = 0; i < locs.loc.length; i++)
            arr.push(locArray(locs.loc[i]));
        return arr;
    } else {
        arr = [];
        for (var i = 0; i < locs.length; i++)
            arr.push(locArray(locs[i].loc));
        return arr;
    }
};

var minBoxSize = function(env, box) {
    return env.bucketSize * Math.pow(2, minBucketScale(env, box));
};

var minBucketScale = function(env, box) {

    if (box.length && box[0].length)
        box = [box[0][0] - box[1][0], box[0][1] - box[1][1]];

    if (box.length)
        box = Math.max(box[0], box[1]);

    print(box);
    print(env.bucketSize);

    return Math.ceil(Math.log(box / env.bucketSize) / Math.log(2));

};

// TODO:  Add spherical $uniqueDocs tests
var numTests = 100;

// Our seed will change every time this is run, but
// each individual test will be reproducible given
// that seed and test number
var seed = new Date().getTime();
// seed = 175 + 288 + 12

for (var test = 0; test < numTests; test++) {
    Random.srand(seed + test);
    // Random.srand( 42240 )
    // Random.srand( 7344 )
    var t = db.testAllGeo;
    t.drop();

    print("Generating test environment #" + test);
    var env = randEnvironment();
    // env.bits = 11
    var query = randQuery(env);
    var data = randDataType();
    // data.numDocs = 5; data.maxLocs = 1;
    var paddingSize = Math.floor(Random.rand() * 10 + 1);
    var results = {};
    var totalPoints = 0;
    print("Calculating target results for " + data.numDocs + " docs with max " + data.maxLocs +
          " locs ");

    var bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < data.numDocs; i++) {
        var numLocs = Math.floor(Random.rand() * data.maxLocs + 1);
        totalPoints += numLocs;

        var multiPoint = [];
        for (var p = 0; p < numLocs; p++) {
            var point = randPoint(env, query);
            multiPoint.push(point);
        }

        var indResults = queryResults(multiPoint, query, results);

        var doc;
        // Nest the keys differently
        if (Random.rand() < 0.5)
            doc = {locs: {loc: randLocTypes(multiPoint)}};
        else
            doc = {locs: randLocTypes(multiPoint, "loc")};

        randQueryAdditions(doc, indResults);

        doc._id = i;
        bulk.insert(doc);
    }
    assert.writeOK(bulk.execute());

    var indexDoc = {"locs.loc": "2d"};
    randIndexAdditions(indexDoc);

    // "earth" is used to drive test setup and not a valid createIndexes option or required at this
    // point. It must be removed before calling ensureIndexes().
    delete env.earth;

    assert.commandWorked(t.ensureIndex(indexDoc, env));
    assert.isnull(db.getLastError());

    var padding = "x";
    for (var i = 0; i < paddingSize; i++)
        padding = padding + padding;

    print(padding);

    printjson({
        seed: seed,
        test: test,
        env: env,
        query: query,
        data: data,
        results: results,
        paddingSize: paddingSize
    });

    // exact
    print("Exact query...");
    assert.eq(
        results.exact.docsIn,
        t.find({"locs.loc": randLocType(query.exact), "exact.docIn": randYesQuery()}).count());

    // $center
    print("Center query...");
    print("Min box : " + minBoxSize(env, query.radius));
    assert.eq(results.center.docsIn,
              t.find({
                   "locs.loc": {$within: {$center: [query.center, query.radius], $uniqueDocs: 1}},
                   "center.docIn": randYesQuery()
               }).count());

    print("Center query update...");
    var res = t.update({
        "locs.loc": {$within: {$center: [query.center, query.radius], $uniqueDocs: true}},
        "center.docIn": randYesQuery()
    },
                       {$set: {centerPaddingA: padding}},
                       false,
                       true);
    assert.eq(results.center.docsIn, res.nModified);

    if (query.sphereRadius >= 0) {
        print("Center sphere query...");
        // $centerSphere
        assert.eq(
            results.sphere.docsIn,
            t.find({
                 "locs.loc": {$within: {$centerSphere: [query.sphereCenter, query.sphereRadius]}},
                 "sphere.docIn": randYesQuery()
             }).count());

        print("Center sphere query update...");
        res = t.update({
            "locs.loc": {
                $within:
                    {$centerSphere: [query.sphereCenter, query.sphereRadius], $uniqueDocs: true}
            },
            "sphere.docIn": randYesQuery()
        },
                       {$set: {spherePaddingA: padding}},
                       false,
                       true);
        assert.eq(results.sphere.docsIn, res.nModified);
    }

    // $box
    print("Box query...");
    assert.eq(results.box.docsIn, t.find({
                                       "locs.loc": {$within: {$box: query.box, $uniqueDocs: true}},
                                       "box.docIn": randYesQuery()
                                   }).count());

    // $polygon
    print("Polygon query...");
    assert.eq(results.poly.docsIn, t.find({
                                        "locs.loc": {$within: {$polygon: query.boxPoly}},
                                        "poly.docIn": randYesQuery()
                                    }).count());

    var defaultDocLimit = 100;

    // $near
    print("Near query...");
    assert.eq(results.center.docsIn,
              t.find({"locs.loc": {$near: query.center, $maxDistance: query.radius}}).count(true),
              "Near query: center: " + query.center + "; radius: " + query.radius + "; docs: " +
                  results.center.docsIn + "; locs: " + results.center.locsIn);

    if (query.sphereRadius >= 0) {
        print("Near sphere query...");
        // $centerSphere
        assert.eq(
            results.sphere.docsIn,
            t.find({
                 "locs.loc": {$nearSphere: query.sphereCenter, $maxDistance: query.sphereRadius}
             }).count(true),
            "Near sphere query: sphere center: " + query.sphereCenter + "; radius: " +
                query.sphereRadius + "; docs: " + results.sphere.docsIn + "; locs: " +
                results.sphere.locsIn);
    }

    // geoNear
    // results limited by size of objects
    if (data.maxLocs < defaultDocLimit) {
        // GeoNear query
        print("GeoNear query...");
        // GeoNear command has a default doc limit 100.
        assert.eq(
            Math.min(defaultDocLimit, results.center.docsIn),
            t.getDB()
                .runCommand({geoNear: "testAllGeo", near: query.center, maxDistance: query.radius})
                .results.length,
            "GeoNear query: center: " + query.center + "; radius: " + query.radius + "; docs: " +
                results.center.docsIn + "; locs: " + results.center.locsIn);

        var num = Math.min(2 * defaultDocLimit, 2 * results.center.docsIn);

        var output = db.runCommand({
                           geoNear: "testAllGeo",
                           near: query.center,
                           maxDistance: query.radius,
                           includeLocs: true,
                           num: num
                       }).results;

        assert.eq(Math.min(num, results.center.docsIn),
                  output.length,
                  "GeoNear query with limit of " + num + ": center: " + query.center +
                      "; radius: " + query.radius + "; docs: " + results.center.docsIn +
                      "; locs: " + results.center.locsIn);

        var distance = 0;
        for (var i = 0; i < output.length; i++) {
            var retDistance = output[i].dis;
            var retLoc = locArray(output[i].loc);

            var arrLocs = locsArray(output[i].obj.locs);

            assert.contains(retLoc, arrLocs);

            var distInObj = false;
            for (var j = 0; j < arrLocs.length && distInObj == false; j++) {
                var newDistance = Geo.distance(locArray(query.center), arrLocs[j]);
                distInObj =
                    (newDistance >= retDistance - 0.0001 && newDistance <= retDistance + 0.0001);
            }

            assert(distInObj);
            assert.between(retDistance - 0.0001,
                           Geo.distance(locArray(query.center), retLoc),
                           retDistance + 0.0001);
            assert.lte(retDistance, query.radius);
            assert.gte(retDistance, distance);
            distance = retDistance;
        }
    }

    // $polygon
    print("Polygon remove...");
    res =
        t.remove({"locs.loc": {$within: {$polygon: query.boxPoly}}, "poly.docIn": randYesQuery()});
    assert.eq(results.poly.docsIn, res.nRemoved);
}

testServer.stop();
