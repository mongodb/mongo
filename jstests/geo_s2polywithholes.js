var t = db.geo_s2weirdpolys;
t.drop();
t.ensureIndex({geo: "2dsphere"});

var centerPoint = {"type": "Point", "coordinates": [0.5, 0.5]};
var edgePoint = {"type": "Point", "coordinates": [0, 0.5]};
var cornerPoint = {"type": "Point", "coordinates": [0, 0]};

t.insert({geo : centerPoint});
t.insert({geo : edgePoint});
t.insert({geo : cornerPoint});

var polygonWithNoHole = {"type" : "Polygon", "coordinates": [
        [[0,0], [0,1], [1, 1], [1, 0], [0, 0]]
    ]
};

// Test 1: Sanity check.  Expect all three points.
var sanityResult = t.find({geo: {$within: {$geometry: polygonWithNoHole}}});
assert.eq(sanityResult.count(), 3);

// Test 2: Polygon with a hole that isn't contained byt the poly shell.
var polygonWithProtrudingHole = {"type" : "Polygon", "coordinates": [
        [[0,0], [0,1], [1, 1], [1, 0], [0, 0]],
        [[0.4,0.9], [0.4,1.1], [0.5, 1.1], [0.5, 0.9], [0.4, 0.9]]
    ]
};

// Bad shell, should error.
t.insert({geo: polygonWithProtrudingHole});
assert(db.getLastError());

// Can't search with bogus poly.
assert.throws(function() {
    return t.find({geo: {$within: {$geometry: polygonWithProtrudingHole}}}).count()
})

// Test 3: This test will confirm that a polygon with overlapping holes throws
// an error.
var polyWithOverlappingHoles = {"type" : "Polygon", "coordinates": [
        [[0,0], [0,1], [1, 1], [1, 0], [0, 0]],
        [[0.2,0.6], [0.2,0.9], [0.6, 0.9], [0.6, 0.6], [0.2, 0.6]],
        [[0.5,0.4], [0.5,0.7], [0.8, 0.7], [0.8, 0.4], [0.5, 0.4]]
    ]
};

t.insert({geo: polyWithOverlappingHoles});
assert(db.getLastError());
