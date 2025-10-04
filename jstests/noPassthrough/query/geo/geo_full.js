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

import {deg2rad, rad2deg} from "jstests/libs/query/geo_math.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod failed to start.");
const db = conn.getDB("test");

let randEnvironment = function () {
    // Normal earth environment
    if (Random.rand() < 0.5) {
        return {max: 180, min: -180, bits: Math.floor(Random.rand() * 32) + 1, earth: true};
    }

    let scales = [0.0001, 0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000, 100000];
    let scale = scales[Math.floor(Random.rand() * scales.length)];
    let offset = Random.rand() * scale;

    let max = Random.rand() * scale + offset;
    let min = -Random.rand() * scale + offset;
    let bits = Math.floor(Random.rand() * 32) + 1;

    return {max: max, min: min, bits: bits, earth: false};
};

let randPoint = function (env, query) {
    if (query && Random.rand() > 0.5) return query.exact;

    if (env.earth) return [Random.rand() * 360 - 180, Random.rand() * 180 - 90];

    let range = env.max - env.min;
    return [Random.rand() * range + env.min, Random.rand() * range + env.min];
};

let randLocType = function (loc, wrapIn) {
    return randLocTypes([loc], wrapIn)[0];
};

var randLocTypes = function (locs, wrapIn) {
    let rLocs = [];

    for (var i = 0; i < locs.length; i++) {
        rLocs.push(locs[i]);
    }

    if (wrapIn) {
        let wrappedLocs = [];
        for (var i = 0; i < rLocs.length; i++) {
            let wrapper = {};
            wrapper[wrapIn] = rLocs[i];
            wrappedLocs.push(wrapper);
        }

        return wrappedLocs;
    }

    return rLocs;
};

let randDataType = function () {
    let scales = [1, 10, 100, 1000, 10000];
    let docScale = scales[Math.floor(Random.rand() * scales.length)];
    let locScale = scales[Math.floor(Random.rand() * scales.length)];

    let numDocs = 40000;
    let maxLocs = 40000;
    // Make sure we don't blow past our test resources
    while (numDocs * maxLocs > 40000) {
        numDocs = Math.floor(Random.rand() * docScale) + 1;
        maxLocs = Math.floor(Random.rand() * locScale) + 1;
    }

    return {numDocs: numDocs, maxLocs: maxLocs};
};

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
    let sin_c = Math.cos(deg2rad(latDegrees));
    let sin_a = Math.sin(deg2rad(maxDistDegrees));
    if (sin_a > sin_c) {
        // Double floating number error, return invalid distance
        return 180;
    }
    let angleA = Math.asin(sin_a / sin_c);
    return rad2deg(angleA);
}

function errorMarginForPoint(env) {
    if (!env.bits) {
        return 0.01;
    }
    let scalingFactor = Math.pow(2, env.bits);
    return ((env.max - env.min) / scalingFactor) * Math.sqrt(2);
}

function pointIsOK(startPoint, radius, env) {
    let error = errorMarginForPoint(env);
    let distDegrees = rad2deg(radius) + error;
    // TODO SERVER-24440: Points close to the north and south poles may fail to be returned by
    // $nearSphere queries answered using a "2d" index. We have empirically found that points
    // with latitudes between 89 and 90 degrees are potentially affected by this issue, so we
    // additionally reject any coordinates with a latitude that falls within that range.
    if (startPoint[1] + distDegrees > 89 || startPoint[1] - distDegrees < -89) {
        return false;
    }
    let xscandist = computexscandist(startPoint[1], distDegrees);
    return startPoint[0] + xscandist < 180 && startPoint[0] - xscandist > -180;
}

let randQuery = function (env) {
    let center = randPoint(env);

    let sphereRadius = -1;
    let sphereCenter = null;
    if (env.earth) {
        // Get a start point that doesn't require wrapping
        // TODO: Are we a bit too aggressive with wrapping issues?
        let i;
        for (i = 0; i < 5; i++) {
            sphereRadius = (Random.rand() * 45 * Math.PI) / 180;
            sphereCenter = randPoint(env);
            if (pointIsOK(sphereCenter, sphereRadius, env)) {
                break;
            }
        }
        if (i == 5) sphereRadius = -1;
    }

    let box = [randPoint(env), randPoint(env)];

    let boxPoly = [
        [box[0][0], box[0][1]],
        [box[0][0], box[1][1]],
        [box[1][0], box[1][1]],
        [box[1][0], box[0][1]],
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
        boxPoly: boxPoly,
    };
};

let resultTypes = {
    "exact": function (loc) {
        return query.exact[0] == loc[0] && query.exact[1] == loc[1];
    },
    "center": function (loc) {
        return Geo.distance(query.center, loc) <= query.radius;
    },
    "box": function (loc) {
        return (
            loc[0] >= query.box[0][0] &&
            loc[0] <= query.box[1][0] &&
            loc[1] >= query.box[0][1] &&
            loc[1] <= query.box[1][1]
        );
    },
    "sphere": function (loc) {
        return query.sphereRadius >= 0 ? Geo.sphereDistance(query.sphereCenter, loc) <= query.sphereRadius : false;
    },
    "poly": function (loc) {
        return (
            loc[0] >= query.box[0][0] &&
            loc[0] <= query.box[1][0] &&
            loc[1] >= query.box[0][1] &&
            loc[1] <= query.box[1][1]
        );
    },
};

let queryResults = function (locs, query, results) {
    if (!results["center"]) {
        for (var type in resultTypes) {
            results[type] = {docsIn: 0, docsOut: 0, locsIn: 0, locsOut: 0};
        }
    }

    let indResults = {};
    for (var type in resultTypes) {
        indResults[type] = {docIn: false, locsIn: 0, locsOut: 0};
    }

    for (var type in resultTypes) {
        let docIn = false;
        for (let i = 0; i < locs.length; i++) {
            if (resultTypes[type](locs[i])) {
                results[type].locsIn++;
                indResults[type].locsIn++;
                indResults[type].docIn = true;
            } else {
                results[type].locsOut++;
                indResults[type].locsOut++;
            }
        }
        if (indResults[type].docIn) results[type].docsIn++;
        else results[type].docsOut++;
    }

    return indResults;
};

let randQueryAdditions = function (doc, indResults) {
    for (let type in resultTypes) {
        let choice = Random.rand();
        if (Random.rand() < 0.25) doc[type] = indResults[type].docIn ? {docIn: "yes"} : {docIn: "no"};
        else if (Random.rand() < 0.5) doc[type] = indResults[type].docIn ? {docIn: ["yes"]} : {docIn: ["no"]};
        else if (Random.rand() < 0.75) doc[type] = indResults[type].docIn ? [{docIn: "yes"}] : [{docIn: "no"}];
        else doc[type] = indResults[type].docIn ? [{docIn: ["yes"]}] : [{docIn: ["no"]}];
    }
};

let randIndexAdditions = function (indexDoc) {
    for (let type in resultTypes) {
        if (Random.rand() < 0.5) continue;

        let choice = Random.rand();
        if (Random.rand() < 0.5) indexDoc[type] = 1;
        else indexDoc[type + ".docIn"] = 1;
    }
};

let randYesQuery = function () {
    let choice = Math.floor(Random.rand() * 7);
    if (choice == 0) return {$ne: "no"};
    else if (choice == 1) return "yes";
    else if (choice == 2) return /^yes/;
    else if (choice == 3) return {$in: ["good", "yes", "ok"]};
    else if (choice == 4) return {$exists: true};
    else if (choice == 5) return {$nin: ["bad", "no", "not ok"]};
    else if (choice == 6) return {$not: /^no/};
};

let locArray = function (loc) {
    if (loc.x) return [loc.x, loc.y];
    if (!loc.length) return [loc[0], loc[1]];
    return loc;
};

let locsArray = function (locs) {
    if (locs.loc) {
        const arr = [];
        for (var i = 0; i < locs.loc.length; i++) arr.push(locArray(locs.loc[i]));
        return arr;
    } else {
        const arr = [];
        for (var i = 0; i < locs.length; i++) arr.push(locArray(locs[i].loc));
        return arr;
    }
};

let minBoxSize = function (env, box) {
    return env.bucketSize * Math.pow(2, minBucketScale(env, box));
};

var minBucketScale = function (env, box) {
    if (box.length && box[0].length) box = [box[0][0] - box[1][0], box[0][1] - box[1][1]];

    if (box.length) box = Math.max(box[0], box[1]);

    print(box);
    print(env.bucketSize);

    return Math.ceil(Math.log(box / env.bucketSize) / Math.log(2));
};

// TODO:  Add spherical $uniqueDocs tests
let numTests = 100;

// Our seed will change every time this is run, but
// each individual test will be reproducible given
// that seed and test number
let seed = new Date().getTime();
// seed = 175 + 288 + 12

for (let test = 0; test < numTests; test++) {
    Random.srand(seed + test);
    // Random.srand( 42240 )
    // Random.srand( 7344 )
    let t = db.testAllGeo;
    t.drop();

    print("Generating test environment #" + test);
    let env = randEnvironment();
    // env.bits = 11
    var query = randQuery(env);
    let data = randDataType();
    // data.numDocs = 5; data.maxLocs = 1;
    let paddingSize = Math.floor(Random.rand() * 10 + 1);
    let results = {};
    let totalPoints = 0;
    print("Calculating target results for " + data.numDocs + " docs with max " + data.maxLocs + " locs ");

    let bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < data.numDocs; i++) {
        let numLocs = Math.floor(Random.rand() * data.maxLocs + 1);
        totalPoints += numLocs;

        let multiPoint = [];
        for (let p = 0; p < numLocs; p++) {
            let point = randPoint(env, query);
            multiPoint.push(point);
        }

        let indResults = queryResults(multiPoint, query, results);

        var doc;
        // Nest the keys differently
        if (Random.rand() < 0.5) doc = {locs: {loc: randLocTypes(multiPoint)}};
        else doc = {locs: randLocTypes(multiPoint, "loc")};

        randQueryAdditions(doc, indResults);

        doc._id = i;
        bulk.insert(doc);
    }
    assert.commandWorked(bulk.execute());

    let indexDoc = {"locs.loc": "2d"};
    randIndexAdditions(indexDoc);

    // "earth" is used to drive test setup and not a valid createIndex option or required at
    // this point. It must be removed before calling createIndex().
    delete env.earth;

    assert.commandWorked(t.createIndex(indexDoc, env));

    let padding = "x";
    for (var i = 0; i < paddingSize; i++) padding = padding + padding;

    print(padding);

    printjson({
        seed: seed,
        test: test,
        env: env,
        query: query,
        data: data,
        results: results,
        paddingSize: paddingSize,
    });

    // exact
    print("Exact query...");
    assert.eq(
        results.exact.docsIn,
        t.find({"locs.loc": randLocType(query.exact), "exact.docIn": randYesQuery()}).count(),
    );

    // $center
    print("Center query...");
    print("Min box : " + minBoxSize(env, query.radius));
    assert.eq(
        results.center.docsIn,
        t
            .find({
                "locs.loc": {$within: {$center: [query.center, query.radius], $uniqueDocs: 1}},
                "center.docIn": randYesQuery(),
            })
            .count(),
    );

    print("Center query update...");
    let res = t.update(
        {
            "locs.loc": {$within: {$center: [query.center, query.radius], $uniqueDocs: true}},
            "center.docIn": randYesQuery(),
        },
        {$set: {centerPaddingA: padding}},
        false,
        true,
    );
    assert.eq(results.center.docsIn, res.nModified);

    if (query.sphereRadius >= 0) {
        print("Center sphere query...");
        // $centerSphere
        assert.eq(
            results.sphere.docsIn,
            t
                .find({
                    "locs.loc": {$within: {$centerSphere: [query.sphereCenter, query.sphereRadius]}},
                    "sphere.docIn": randYesQuery(),
                })
                .count(),
        );

        print("Center sphere query update...");
        res = t.update(
            {
                "locs.loc": {
                    $within: {$centerSphere: [query.sphereCenter, query.sphereRadius], $uniqueDocs: true},
                },
                "sphere.docIn": randYesQuery(),
            },
            {$set: {spherePaddingA: padding}},
            false,
            true,
        );
        assert.eq(results.sphere.docsIn, res.nModified);
    }

    // $box
    print("Box query...");
    assert.eq(
        results.box.docsIn,
        t
            .find({
                "locs.loc": {$within: {$box: query.box, $uniqueDocs: true}},
                "box.docIn": randYesQuery(),
            })
            .count(),
    );

    // $polygon
    print("Polygon query...");
    assert.eq(
        results.poly.docsIn,
        t
            .find({
                "locs.loc": {$within: {$polygon: query.boxPoly}},
                "poly.docIn": randYesQuery(),
            })
            .count(),
    );

    // $near
    print("Near query...");
    assert.eq(
        results.center.docsIn,
        t.find({"locs.loc": {$near: query.center, $maxDistance: query.radius}}).count(true),
        "Near query: center: " +
            query.center +
            "; radius: " +
            query.radius +
            "; docs: " +
            results.center.docsIn +
            "; locs: " +
            results.center.locsIn,
    );

    if (query.sphereRadius >= 0) {
        print("Near sphere query...");
        // $centerSphere
        assert.eq(
            results.sphere.docsIn,
            t
                .find({
                    "locs.loc": {$nearSphere: query.sphereCenter, $maxDistance: query.sphereRadius},
                })
                .count(true),
            "Near sphere query: sphere center: " +
                query.sphereCenter +
                "; radius: " +
                query.sphereRadius +
                "; docs: " +
                results.sphere.docsIn +
                "; locs: " +
                results.sphere.locsIn,
        );
    }

    // $geoNear aggregation stage.
    const aggregationLimit = 2 * results.center.docsIn;
    if (aggregationLimit > 0) {
        let output = t
            .aggregate([
                {
                    $geoNear: {
                        near: query.center,
                        maxDistance: query.radius,
                        includeLocs: "pt",
                        distanceField: "dis",
                    },
                },
                {$limit: aggregationLimit},
            ])
            .toArray();

        const errmsg = {
            limit: aggregationLimit,
            center: query.center,
            radius: query.radius,
            docs: results.center.docsIn,
            locs: results.center.locsIn,
            actualResult: output,
        };
        assert.eq(results.center.docsIn, output.length, tojson(errmsg));

        let lastDistance = 0;
        for (var i = 0; i < output.length; i++) {
            let retDistance = output[i].dis;
            assert.close(retDistance, Geo.distance(locArray(query.center), output[i].pt));
            assert.lte(retDistance, query.radius);
            assert.gte(retDistance, lastDistance);
            lastDistance = retDistance;
        }
    }

    // $polygon
    print("Polygon remove...");
    res = t.remove({"locs.loc": {$within: {$polygon: query.boxPoly}}, "poly.docIn": randYesQuery()});
    assert.eq(results.poly.docsIn, res.nRemoved);
}

MongoRunner.stopMongod(conn);
