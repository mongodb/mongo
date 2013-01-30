// A test for what geometries can interact with what other geometries.
t = db.geo_allowedcomparisons;

// Any GeoJSON object can intersect with any geojson object.
geojsonPoint = { "type" : "Point", "coordinates": [ 0, 0 ] };
oldPoint = [0,0];

// GeoJSON polygons can contain any geojson object and OLD points.
geojsonPoly = { "type" : "Polygon",
                "coordinates" : [ [ [-5,-5], [-5,5], [5,5], [5,-5], [-5,-5]]]};

// This can be contained by GJ polygons, intersected by anything GJ and old points.
geojsonLine = { "type" : "LineString", "coordinates": [ [ 0, 0], [1, 1]]}

// $centerSphere can contain old or new points.
oldCenterSphere = [[0, 0], Math.PI / 180];
// $box can contain old points.
oldBox = [[-5,-5], [5,5]];
// $polygon can contain old points.
oldPolygon = [[-5,-5], [-5,5], [5,5], [5,-5], [-5,-5]]
// $center can contain old points.
oldCenter = [[0, 0], 1];

t.drop();
t.ensureIndex({geo: "2d"});
// 2d doesn't know what to do w/this
t.insert({geo: geojsonPoint});
assert(db.getLastError());
// Old points are OK.
t.insert({geo: oldPoint})
assert(!db.getLastError());
// Lines not OK in 2d
t.insert({geo: geojsonLine})
assert(db.getLastError())
// Shapes are not OK to insert in 2d
t.insert({geo: geojsonPoly})
assert(db.getLastError());
t.insert({geo: oldCenterSphere})
assert(db.getLastError());
t.insert({geo: oldCenter})
assert(db.getLastError());
// If we try to insert a polygon, it thinks it's an array of points.  Let's not
// do that.  Ditto for the box.

// Verify that even if we can't index them, we can use them in a matcher.
t.insert({gj: geojsonLine})
t.insert({gj: geojsonPoly})
geojsonPoint2 = { "type" : "Point", "coordinates": [ 0, 0.001 ] };
t.insert({gjp: geojsonPoint2})

// We convert between old and new style points.
assert.eq(1, t.find({gjp: {$within: {$box: oldBox}}}).itcount());
assert.eq(1, t.find({gjp: {$within: {$polygon: oldPolygon}}}).itcount());
assert.eq(1, t.find({gjp: {$within: {$center: oldCenter}}}).itcount());
assert.eq(1, t.find({gjp: {$within: {$centerSphere: oldCenterSphere}}}).itcount())

function runTests() {
    // Each find the box, the polygon, and the old point.
    assert.eq(1, t.find({geo: {$within: {$box: oldBox}}}).itcount())
    assert.eq(1, t.find({geo: {$within: {$polygon: oldPolygon}}}).itcount())
    // Each find the old point.
    assert.eq(1, t.find({geo: {$within: {$center: oldCenter}}}).itcount())
    assert.eq(1, t.find({geo: {$within: {$centerSphere: oldCenterSphere}}}).itcount())
    // Using geojson with 2d-style within syntax should choke.
    assert.throws(function() { return t.find({geo: {$within: {$polygon: geojsonPoly}}})
                                       .itcount();})
    // Using old polygon w/new syntax should choke too.
    assert.throws(function() { return t.find({geo: {$within: {$geometry: oldPolygon}}})
                                       .itcount();})
    assert.throws(function() { return t.find({geo: {$within: {$geometry: oldBox}}})
                                       .itcount();})
    assert.throws(function() { return t.find({geo: {$within: {$geometry: oldCenter}}})
                                       .itcount();})
    assert.throws(function() { return t.find({geo: {$within: {$geometry: oldCenterSphere}}})
                                       .itcount();})
    // Even if we only have a 2d index, the 2d suitability function should
    // allow the matcher to deal with this.  If we have a 2dsphere index we use it.
    assert.eq(1, t.find({geo: {$within: {$geometry: geojsonPoly}}}).itcount())
    assert.eq(1, t.find({geo: {$geoIntersects: {$geometry: geojsonPoly}}}).itcount())
    assert.eq(1, t.find({geo: {$geoIntersects: {$geometry: oldPoint}}}).itcount())
    assert.eq(1, t.find({geo: {$geoIntersects: {$geometry: geojsonPoint}}}).itcount())
}

// We have a 2d index right now.  Let's see what it does.
runTests();

// No index now.
t.dropIndex({geo: "2d"})
runTests();

// 2dsphere index now.
t.ensureIndex({geo: "2dsphere"})
assert(!db.getLastError())
// 2dsphere does not support arrays of points.
t.insert({geo: [geojsonPoint2, geojsonPoint]})
assert(db.getLastError())
runTests();

// Old stuff is not GeoJSON (or old-style point).  All should fail.
t.insert({geo: oldBox})
assert(db.getLastError())
t.insert({geo: oldPolygon})
assert(db.getLastError())
t.insert({geo: oldCenter})
assert(db.getLastError())
t.insert({geo: oldCenterSphere})
assert(db.getLastError())
