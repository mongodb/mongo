// A test for what geometries can interact with what other geometries.
t = db.geo_allowedcomparisons;

// Any GeoJSON object can intersect with any geojson object.
geojsonPoint = {
    "type": "Point",
    "coordinates": [0, 0]
};
oldPoint = [0, 0];

// GeoJSON polygons can contain any geojson object and OLD points.
geojsonPoly = {
    "type": "Polygon",
    "coordinates": [[[-5, -5], [-5, 5], [5, 5], [5, -5], [-5, -5]]]
};

// This can be contained by GJ polygons, intersected by anything GJ and old points.
geojsonLine = {
    "type": "LineString",
    "coordinates": [[0, 0], [1, 1]]
};

// $centerSphere can contain old or new points.
oldCenterSphere = [[0, 0], Math.PI / 180];
// $box can contain old points.
oldBox = [[-5, -5], [5, 5]];
// $polygon can contain old points.
oldPolygon = [[-5, -5], [-5, 5], [5, 5], [5, -5], [-5, -5]];
// $center can contain old points.
oldCenter = [[0, 0], 1];

t.drop();
t.ensureIndex({geo: "2d"});
// 2d doesn't know what to do w/this
assert.writeError(t.insert({geo: geojsonPoint}));
// Old points are OK.
assert.writeOK(t.insert({geo: oldPoint}));
// Lines not OK in 2d
assert.writeError(t.insert({geo: geojsonLine}));
// Shapes are not OK to insert in 2d
assert.writeError(t.insert({geo: geojsonPoly}));
assert.writeError(t.insert({geo: oldCenterSphere}));
assert.writeError(t.insert({geo: oldCenter}));
// If we try to insert a polygon, it thinks it's an array of points.  Let's not
// do that.  Ditto for the box.

// Verify that even if we can't index them, we can use them in a matcher.
t.insert({gj: geojsonLine});
t.insert({gj: geojsonPoly});
geojsonPoint2 = {
    "type": "Point",
    "coordinates": [0, 0.001]
};
t.insert({gjp: geojsonPoint2});

// We convert between old and new style points.
assert.eq(1, t.find({gjp: {$geoWithin: {$box: oldBox}}}).itcount());
assert.eq(1, t.find({gjp: {$geoWithin: {$polygon: oldPolygon}}}).itcount());
assert.eq(1, t.find({gjp: {$geoWithin: {$center: oldCenter}}}).itcount());
assert.eq(1, t.find({gjp: {$geoWithin: {$centerSphere: oldCenterSphere}}}).itcount());

function runTests() {
    // Each find the box, the polygon, and the old point.
    assert.eq(1, t.find({geo: {$geoWithin: {$box: oldBox}}}).itcount());
    assert.eq(1, t.find({geo: {$geoWithin: {$polygon: oldPolygon}}}).itcount());
    // Each find the old point.
    assert.eq(1, t.find({geo: {$geoWithin: {$center: oldCenter}}}).itcount());
    assert.eq(1, t.find({geo: {$geoWithin: {$centerSphere: oldCenterSphere}}}).itcount());
    // Using geojson with 2d-style geoWithin syntax should choke.
    assert.throws(function() {
        return t.find({geo: {$geoWithin: {$polygon: geojsonPoly}}}).itcount();
    });
    // Using old polygon w/new syntax should choke too.
    assert.throws(function() {
        return t.find({geo: {$geoWithin: {$geometry: oldPolygon}}}).itcount();
    });
    assert.throws(function() {
        return t.find({geo: {$geoWithin: {$geometry: oldBox}}}).itcount();
    });
    assert.throws(function() {
        return t.find({geo: {$geoWithin: {$geometry: oldCenter}}}).itcount();
    });
    assert.throws(function() {
        return t.find({geo: {$geoWithin: {$geometry: oldCenterSphere}}}).itcount();
    });
    // Even if we only have a 2d index, the 2d suitability function should
    // allow the matcher to deal with this.  If we have a 2dsphere index we use it.
    assert.eq(1, t.find({geo: {$geoWithin: {$geometry: geojsonPoly}}}).itcount());
    assert.eq(1, t.find({geo: {$geoIntersects: {$geometry: geojsonPoly}}}).itcount());
    assert.eq(1, t.find({geo: {$geoIntersects: {$geometry: oldPoint}}}).itcount());
    assert.eq(1, t.find({geo: {$geoIntersects: {$geometry: geojsonPoint}}}).itcount());
}

// We have a 2d index right now.  Let's see what it does.
runTests();

// No index now.
t.dropIndex({geo: "2d"});
runTests();

// 2dsphere index now.
assert.commandWorked(t.ensureIndex({geo: "2dsphere"}));
// 2dsphere does not support arrays of points.
assert.writeError(t.insert({geo: [geojsonPoint2, geojsonPoint]}));
runTests();

// Old stuff is not GeoJSON (or old-style point).  All should fail.
assert.writeError(t.insert({geo: oldBox}));
assert.writeError(t.insert({geo: oldPolygon}));
assert.writeError(t.insert({geo: oldCenter}));
assert.writeError(t.insert({geo: oldCenterSphere}));
