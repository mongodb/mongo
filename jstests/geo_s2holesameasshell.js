// If polygons have holes, the holes cannot be equal to the entire geometry.
var t = db.geo_s2holessameasshell
t.drop();
t.ensureIndex({geo: "2dsphere"});

var centerPoint = {"type": "Point", "coordinates": [0.5, 0.5]};
var edgePoint = {"type": "Point", "coordinates": [0, 0.5]};
var cornerPoint = {"type": "Point", "coordinates": [0, 0]};

// Various "edge" cases.  None of them should be returned by the non-polygon
// polygon below.
t.insert({geo : centerPoint});
t.insert({geo : edgePoint});
t.insert({geo : cornerPoint});

// This generates an empty covering.
var polygonWithFullHole = { "type" : "Polygon", "coordinates": [
        [[0,0], [0,1], [1, 1], [1, 0], [0, 0]],
        [[0,0], [0,1], [1, 1], [1, 0], [0, 0]]
    ]
};

// No keys for insert should error.
t.insert({geo: polygonWithFullHole})
assert(db.getLastError())

// No covering to search over should give an empty result set.
assert.throws(function() {
    return t.find({geo: {$geoWithin: {$geometry: polygonWithFullHole}}}).count()})

// Similar polygon to the one above, but is covered by two holes instead of
// one.
var polygonWithTwoHolesCoveringWholeArea = {"type" : "Polygon", "coordinates": [
        [[0,0], [0,1], [1, 1], [1, 0], [0, 0]],
        [[0,0], [0,0.5], [1, 0.5], [1, 0], [0, 0]],
        [[0,0.5], [0,1], [1, 1], [1, 0.5], [0, 0.5]]
    ]
};

// No keys for insert should error.
t.insert({geo: polygonWithTwoHolesCoveringWholeArea});
assert(db.getLastError());

// No covering to search over should give an empty result set.
assert.throws(function() {
    return t.find({geo: {$geoWithin: {$geometry: polygonWithTwoHolesCoveringWholeArea}}}).count()})
