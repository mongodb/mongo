/* Test $minDistance option for $near and $nearSphere queries, and geoNear
 * command. SERVER-9395.
*/
var t = db.geo_mindistance;
t.drop();

//
// Useful constants and functions.
//

var km = 1000,
    earthRadiusMeters = 6378.1 * km;

function metersToRadians(m) { return m / earthRadiusMeters; }

/* Count documents within some radius of (0, 0), in kilometers.
 * With this function we can use the existing $maxDistance option to test
 * the newer $minDistance option's behavior.
 */
function n_docs_within(radius_km) {
    // geoNear's distances are in meters for geoJSON points.
    var cmdResult = db.runCommand({
        geoNear: t.getName(),
        near: {type: 'Point', coordinates: [0, 0]},
        spherical: true,
        maxDistance: radius_km * km,
        num: 1000
    });

    return cmdResult.results.length;
}

//
// Setup.
//

/* Make 121 points from long, lat = (0, 0) (in Gulf of Guinea) to (10, 10)
 * (inland Nigeria).
 */
for (var x = 0; x <= 10; x += 1) {
    for (var y = 0; y <= 10; y += 1) {
        t.insert({loc: [x, y]});
    }
}

/* $minDistance is supported for 2dsphere index only, not 2d or geoHaystack. */
t.ensureIndex({loc: "2dsphere"});

var n_docs = t.count(),
    geoJSONPoint = {$geometry: {type: 'Point', coordinates: [0, 0]}},
    legacyPoint = [0, 0];

//
// Test $near with GeoJSON point (required for $near with 2dsphere index).
// min/maxDistance are in meters.
//

var n_min1400_count = t.find({loc: {
    $near: geoJSONPoint, $minDistance: 1400 * km
}}).count();

assert.eq(
    n_docs - n_docs_within(1400),
    n_min1400_count,
    "Expected " + (n_docs - n_docs_within(1400))
        + " points $near (0, 0) with $minDistance 1400 km, got "
        + n_min1400_count
);

var n_bw500_and_1000_count = t.find({loc: {
    $near: geoJSONPoint,
    $minDistance: 500 * km,
    $maxDistance: 1000 * km
}}).count();

assert.eq(
    n_docs_within(1000) - n_docs_within(500),
    n_bw500_and_1000_count,
    "Expected " + (n_docs_within(1000) - n_docs_within(500))
        + " points $near (0, 0) with $minDistance 500 km and $maxDistance 1000 km, got "
        + n_bw500_and_1000_count
);

//
// $nearSphere with 2dsphere index can take a legacy or GeoJSON point.
// First test $nearSphere with legacy point.
// min/maxDistance are in radians.
//

n_min1400_count = t.find({loc: {
    $nearSphere: legacyPoint, $minDistance: metersToRadians(1400 * km)
}}).count();

assert.eq(
    n_docs - n_docs_within(1400),
    n_min1400_count,
    "Expected " + (n_docs - n_docs_within(1400))
        + " points $nearSphere (0, 0) with $minDistance 1400 km, got "
        + n_min1400_count
);

n_bw500_and_1000_count = t.find({loc: {
    $nearSphere: legacyPoint,
    $minDistance: metersToRadians(500 * km),
    $maxDistance: metersToRadians(1000 * km)
}}).count();

assert.eq(
    n_docs_within(1000) - n_docs_within(500),
    n_bw500_and_1000_count,
    "Expected " + (n_docs_within(1000) - n_docs_within(500))
        + " points $nearSphere (0, 0) with $minDistance 500 km and $maxDistance 1000 km, got "
        + n_bw500_and_1000_count
);

//
// Test $nearSphere with GeoJSON point.
// min/maxDistance are in meters.
//

n_min1400_count = t.find({loc: {
    $nearSphere: geoJSONPoint, $minDistance: 1400 * km
}}).count();

assert.eq(
    n_docs - n_docs_within(1400),
    n_min1400_count,
    "Expected " + (n_docs - n_docs_within(1400))
        + " points $nearSphere (0, 0) with $minDistance 1400 km, got "
        + n_min1400_count
);

n_bw500_and_1000_count = t.find({loc: {
    $nearSphere: geoJSONPoint,
    $minDistance: 500 * km,
    $maxDistance: 1000 * km
}}).count();

assert.eq(
    n_docs_within(1000) - n_docs_within(500),
    n_bw500_and_1000_count,
    "Expected " + (n_docs_within(1000) - n_docs_within(500))
        + " points $nearSphere (0, 0) with $minDistance 500 km and $maxDistance 1000 km, got "
        + n_bw500_and_1000_count
);


//
// Test geoNear command with GeoJSON point.
// Distances are in meters.
//

var cmdResult = db.runCommand({
    geoNear: t.getName(),
    near: {type: 'Point', coordinates: [0, 0]},
    minDistance: 1400 * km,
    spherical: true  // spherical required for 2dsphere index
});
assert.eq(
    n_docs - n_docs_within(1400),
    cmdResult.results.length,
    "Expected " + (n_docs - n_docs_within(1400))
        + " points geoNear (0, 0) with $minDistance 1400 km, got "
        + cmdResult.results.length
);

cmdResult = db.runCommand({
    geoNear: t.getName(),
    near: {type: 'Point', coordinates: [0, 0]},
    minDistance: 500 * km,
    maxDistance: 1000 * km,
    spherical: true
});
assert.eq(
    n_docs_within(1000) - n_docs_within(500),
    cmdResult.results.length,
    "Expected " + (n_docs_within(1000) - n_docs_within(500))
        + " points geoNear (0, 0) with $minDistance 500 km and $maxDistance 1000 km, got "
        + cmdResult.results.length
);

//
// Test geoNear command with legacy point.
// Distances are in radians.
//

cmdResult = db.runCommand({
    geoNear: t.getName(),
    near: legacyPoint,
    minDistance: metersToRadians(1400 * km),
    spherical: true  // spherical required for 2dsphere index
});
assert.eq(
    n_docs - n_docs_within(1400),
    cmdResult.results.length,
    "Expected " + (n_docs - n_docs_within(1400))
        + " points geoNear (0, 0) with $minDistance 1400 km, got "
        + cmdResult.results.length
);

cmdResult = db.runCommand({
    geoNear: t.getName(),
    near: legacyPoint,
    minDistance: metersToRadians(500 * km),
    maxDistance: metersToRadians(1000 * km),
    spherical: true
});
assert.eq(
    n_docs_within(1000) - n_docs_within(500),
    cmdResult.results.length,
    "Expected " + (n_docs_within(1000) - n_docs_within(500))
        + " points geoNear (0, 0) with $minDistance 500 km and $maxDistance 1000 km, got "
        + cmdResult.results.length
);

//
// Verify that we throw errors using 2d index with $minDistance.
// ($minDistance requires a 2dsphere index, not supported with 2d.)
//

t.dropIndexes();
t.ensureIndex({loc: "2d"});

assert.throws(
    function() {
        t.find({loc: {$near: legacyPoint, $minDistance: 1}}).next();
    }, [],
    "$near with legacy coordinates and $minDistance should've failed with 2d index"
);

assert.throws(
    function() {
        t.find({loc: {$near: geoJSONPoint, $minDistance: 1}}).next();
    }, [],
    "$near with GeoJSON point and $minDistance should've failed with 2d index"
);

assert.throws(
    function() {
        t.find({loc: {$nearSphere: legacyPoint, $minDistance: 1}}).next();
    }, [],
    "$nearSphere with legacy coordinates and $minDistance should've failed with 2d index"
);

assert.throws(
    function() {
        t.find({loc: {$nearSphere: geoJSONPoint, $minDistance: 1}}).next();
    }, [],
    "$nearSphere with GeoJSON point and $minDistance should've failed with 2d index"
);

/* geoNear command with 2d index requires legacy point, not GeoJSON.
 * It fails here, even with a legacy point, because of minDistance.
*/
assert.commandFailed(
    db.runCommand({
        geoNear: t.getName(),
        near: legacyPoint,
        minDistance: 1,
        spherical: true
    }),
    "geoNear with legacy coordinates and $minDistance should've failed with 2d index"
);
