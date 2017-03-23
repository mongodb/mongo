/* Test boundary conditions for $minDistance option for $near and $nearSphere
 * queries. SERVER-9395.
*/
var t = db.geo_mindistance_boundaries;
t.drop();
t.insert({loc: [1, 0]});  // 1 degree of longitude from origin.

/* $minDistance is supported for 2dsphere index only, not 2d or geoHaystack. */
t.ensureIndex({loc: "2dsphere"});

//
// Useful constants.
//

var km = 1000, earthRadiusMeters = 6378.1 * km, geoJSONPoint = {type: 'Point', coordinates: [0, 0]},
    // One degree of longitude at the equator, about 111 km.
    degreeInMeters = 2 * Math.PI * earthRadiusMeters / 360, metersEpsilon = Number.MIN_VALUE;

/* Grow epsilon's exponent until epsilon exceeds the margin of error for the
 * representation of degreeInMeters. The server uses 64-bit math, too, so we'll
 * find the smallest epsilon the server can detect.
*/
while (degreeInMeters + metersEpsilon == degreeInMeters) {
    metersEpsilon *= 2;
}

//
// Test boundary conditions for $near and GeoJSON, in meters.
//

// minDistance must be within the args to $near, not on the side.
assert.throws(function() {
    t.find({loc: {$near: {$geometry: geoJSONPoint}, $minDistance: 0.1}}).itcount();
});

assert.eq(1,
          t.find({loc: {$near: {$geometry: geoJSONPoint, $minDistance: degreeInMeters}}}).itcount(),
          "Expected to find (0, 1) within $minDistance 1 degree from origin");

assert.eq(
    1,
    t.find({
         loc: {$near: {$geometry: geoJSONPoint, $minDistance: degreeInMeters - metersEpsilon}}
     }).itcount(),
    "Expected to find (0, 1) within $minDistance (1 degree - epsilon) from origin");

assert.eq(
    0,
    t.find({
         loc: {$near: {$geometry: geoJSONPoint, $minDistance: degreeInMeters + metersEpsilon}}
     }).itcount(),
    "Expected *not* to find (0, 1) within $minDistance (1 degree + epsilon) from origin");

//
// Test boundary conditions for $nearSphere and GeoJSON, in meters.
//

assert.eq(
    1,
    t.find({loc: {$nearSphere: {$geometry: geoJSONPoint, $minDistance: degreeInMeters}}}).itcount(),
    "Expected to find (0, 1) within $minDistance 1 degree from origin");

assert.eq(1,
          t.find({
               loc: {$nearSphere: geoJSONPoint, $minDistance: degreeInMeters - metersEpsilon}
           }).itcount(),
          "Expected to find (0, 1) within $minDistance (1 degree - epsilon) from origin");

assert.eq(0,
          t.find({
               loc: {$nearSphere: geoJSONPoint, $minDistance: degreeInMeters + metersEpsilon}
           }).itcount(),
          "Expected *not* to find (0, 1) within $minDistance (1 degree + epsilon) from origin");

//
// Test boundary conditions for $nearSphere and a legacy point, in radians.
//
// $minDistance with legacy point requires $nearSphere; $near not
// supported.
//

var legacyPoint = [0, 0], degreeInRadians = 2 * Math.PI / 360, radiansEpsilon = Number.MIN_VALUE;

while (1 + radiansEpsilon == 1) {
    radiansEpsilon *= 2;
}

assert.eq(1,
          t.find({loc: {$nearSphere: legacyPoint, $minDistance: degreeInRadians}}).itcount(),
          "Expected to find (0, 1) within $minDistance 1 degree from origin");

assert.eq(1,
          t.find({
               loc: {$nearSphere: legacyPoint, $minDistance: degreeInRadians - radiansEpsilon}
           }).itcount(),
          "Expected to find (0, 1) within $minDistance (1 degree - epsilon) from origin");

assert.eq(0,
          t.find({
               loc: {$nearSphere: legacyPoint, $minDistance: degreeInRadians + radiansEpsilon}
           }).itcount(),
          "Expected *not* to find (0, 1) within $minDistance (1 degree + epsilon) from origin");
