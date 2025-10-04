// @tags: [
//   requires_getmore,
// ]

// Test 2dsphere near search, called via find and $geoNear.
let t = db.geo_s2near;
t.drop();

// Make sure that geoNear gives us back loc
let goldenPoint = {type: "Point", coordinates: [31.0, 41.0]};
t.insert({geo: goldenPoint});
t.createIndex({geo: "2dsphere"});
let resNear = t
    .aggregate([{$geoNear: {near: [30, 40], distanceField: "d", spherical: true, includeLocs: "loc"}}, {$limit: 1}])
    .toArray();
assert.eq(resNear.length, 1, tojson(resNear));
assert.eq(resNear[0].loc, goldenPoint);

// FYI:
// One degree of long @ 0 is 111km or so.
// One degree of lat @ 0 is 110km or so.
let lat = 0;
let lng = 0;
let points = 10;
for (let x = -points; x < points; x += 1) {
    for (let y = -points; y < points; y += 1) {
        t.insert({geo: {"type": "Point", "coordinates": [lng + x / 1000.0, lat + y / 1000.0]}});
    }
}

let origin = {"type": "Point", "coordinates": [lng, lat]};

t.createIndex({geo: "2dsphere"});

// Near only works when the query is a point.
let someline = {
    "type": "LineString",
    "coordinates": [
        [40, 5],
        [41, 6],
    ],
};
let somepoly = {
    "type": "Polygon",
    "coordinates": [
        [
            [40, 5],
            [40, 6],
            [41, 6],
            [41, 5],
            [40, 5],
        ],
    ],
};
assert.throws(function () {
    return t.find({"geo": {"$near": {"$geometry": someline}}}).count();
});
assert.throws(function () {
    return t.find({"geo": {"$near": {"$geometry": somepoly}}}).count();
});
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: t.getName(),
        cursor: {},
        pipeline: [{$geoNear: {near: someline, distanceField: "dis", spherical: true}}],
    }),
    2,
);
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: t.getName(),
        cursor: {},
        pipeline: [{$geoNear: {near: somepoly, distanceField: "dis", spherical: true}}],
    }),
    2,
);

// Do some basic near searches.
let res = t.find({"geo": {"$near": {"$geometry": origin, $maxDistance: 2000}}}).limit(10);
resNear = t.aggregate([
    {$geoNear: {near: [0, 0], distanceField: "dis", maxDistance: Math.PI, spherical: true}},
    {$limit: 10},
]);
assert.eq(res.itcount(), resNear.itcount(), "10");

res = t.find({"geo": {"$near": {"$geometry": origin}}}).limit(10);
resNear = t.aggregate([{$geoNear: {near: [0, 0], distanceField: "dis", spherical: true}}, {$limit: 10}]);
assert.eq(res.itcount(), resNear.itcount(), "10");

// Find all the points!
res = t.find({"geo": {"$near": {"$geometry": origin}}}).limit(10000);
resNear = t.aggregate([{$geoNear: {near: [0, 0], distanceField: "dis", spherical: true}}, {$limit: 10000}]);
assert.eq(res.itcount(), resNear.itcount(), (2 * points * (2 * points)).toString());

// longitude goes -180 to 180
// latitude goes -90 to 90
// Let's put in some perverse (polar) data and make sure we get it back.
// Points go long, lat.
t.insert({geo: {"type": "Point", "coordinates": [-180, -90]}});
t.insert({geo: {"type": "Point", "coordinates": [180, -90]}});
t.insert({geo: {"type": "Point", "coordinates": [180, 90]}});
t.insert({geo: {"type": "Point", "coordinates": [-180, 90]}});
res = t.find({"geo": {"$near": {"$geometry": origin}}}).limit(10000);
resNear = t.aggregate([{$geoNear: {near: [0, 0], distanceField: "dis", spherical: true}}, {$limit: 10000}]);
assert.eq(res.itcount(), resNear.itcount(), (2 * points * (2 * points) + 4).toString());

function testRadAndDegreesOK(distance) {
    // Distance for old style points is radians.
    let resRadians = t.find({geo: {$nearSphere: [0, 0], $maxDistance: distance / (6378.1 * 1000)}});
    // Distance for new style points is meters.
    let resMeters = t.find({"geo": {"$near": {"$geometry": origin, $maxDistance: distance}}});
    // And we should get the same # of results no matter what.
    assert.eq(resRadians.itcount(), resMeters.itcount());

    // Also, $geoNear should behave the same way.
    let resGNMeters = t
        .aggregate({
            $geoNear: {
                near: origin,
                distanceField: "dis",
                maxDistance: distance,
                spherical: true,
            },
        })
        .toArray();
    let resGNRadians = t
        .aggregate({
            $geoNear: {
                near: [0, 0],
                distanceField: "dis",
                maxDistance: distance / (6378.1 * 1000),
                spherical: true,
            },
        })
        .toArray();
    const errmsg =
        `$geoNear using meter distances returned ${tojson(resGNMeters)}, but ` +
        `$geoNear using radian distances returned ${tojson(resGNRadians)}`;
    assert.eq(resGNRadians.length, resGNMeters.length, errmsg);
    for (let i = 0; i < resGNRadians.length; ++i) {
        // Radius of earth * radians = distance in meters.
        assert.close(resGNRadians[i].dis * 6378.1 * 1000, resGNMeters[i].dis);
    }
}

testRadAndDegreesOK(1);
testRadAndDegreesOK(10);
testRadAndDegreesOK(50);
testRadAndDegreesOK(10000);

// SERVER-13666 legacy coordinates must be in bounds for spherical near queries.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: t.getName(),
        cursor: {},
        pipeline: [
            {
                $geoNear: {
                    near: [1210.466, 31.2051],
                    distanceField: "dis",
                    spherical: true,
                },
            },
        ],
    }),
    2,
);
