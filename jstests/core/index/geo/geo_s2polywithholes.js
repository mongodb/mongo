// @tags: [
//   requires_getmore,
// ]

let t = db.geo_s2weirdpolys;
t.drop();
t.createIndex({geo: "2dsphere"});

let centerPoint = {"type": "Point", "coordinates": [0.5, 0.5]};
let edgePoint = {"type": "Point", "coordinates": [0, 0.5]};
let cornerPoint = {"type": "Point", "coordinates": [0, 0]};

t.insert({geo: centerPoint});
t.insert({geo: edgePoint});
t.insert({geo: cornerPoint});

let polygonWithNoHole = {
    "type": "Polygon",
    "coordinates": [
        [
            [0, 0],
            [0, 1],
            [1, 1],
            [1, 0],
            [0, 0],
        ],
    ],
};

// Test 1: Sanity check.  Expect all three points.
let sanityResult = t.find({geo: {$within: {$geometry: polygonWithNoHole}}});
assert.eq(sanityResult.itcount(), 3);

// Test 2: Polygon with a hole that isn't contained byt the poly shell.
let polygonWithProtrudingHole = {
    "type": "Polygon",
    "coordinates": [
        [
            [0, 0],
            [0, 1],
            [1, 1],
            [1, 0],
            [0, 0],
        ],
        [
            [0.4, 0.9],
            [0.4, 1.1],
            [0.5, 1.1],
            [0.5, 0.9],
            [0.4, 0.9],
        ],
    ],
};

// Bad shell, should error.
assert.writeError(t.insert({geo: polygonWithProtrudingHole}));

// Can't search with bogus poly.
assert.throws(function () {
    return t.find({geo: {$within: {$geometry: polygonWithProtrudingHole}}}).itcount();
});

// Test 3: This test will confirm that a polygon with overlapping holes throws
// an error.
let polyWithOverlappingHoles = {
    "type": "Polygon",
    "coordinates": [
        [
            [0, 0],
            [0, 1],
            [1, 1],
            [1, 0],
            [0, 0],
        ],
        [
            [0.2, 0.6],
            [0.2, 0.9],
            [0.6, 0.9],
            [0.6, 0.6],
            [0.2, 0.6],
        ],
        [
            [0.5, 0.4],
            [0.5, 0.7],
            [0.8, 0.7],
            [0.8, 0.4],
            [0.5, 0.4],
        ],
    ],
};

assert.writeError(t.insert({geo: polyWithOverlappingHoles}));

// Test 4: Only one nesting is allowed by GeoJSON.
let polyWithDeepHole = {
    "type": "Polygon",
    "coordinates": [
        [
            [0, 0],
            [0, 1],
            [1, 1],
            [1, 0],
            [0, 0],
        ],
        [
            [0.1, 0.1],
            [0.1, 0.9],
            [0.9, 0.9],
            [0.9, 0.1],
            [0.1, 0.1],
        ],
        [
            [0.2, 0.2],
            [0.2, 0.8],
            [0.8, 0.8],
            [0.8, 0.2],
            [0.2, 0.2],
        ],
    ],
};
assert.writeError(t.insert({geo: polyWithDeepHole}));

// Test 5: The first ring must be the exterior ring.
let polyWithBiggerHole = {
    "type": "Polygon",
    "coordinates": [
        [
            [0.1, 0.1],
            [0.1, 0.9],
            [0.9, 0.9],
            [0.9, 0.1],
            [0.1, 0.1],
        ],
        [
            [0, 0],
            [0, 1],
            [1, 1],
            [1, 0],
            [0, 0],
        ],
    ],
};
assert.writeError(t.insert({geo: polyWithBiggerHole}));

// Test 6: Holes cannot share more than one vertex with exterior loop
let polySharedVertices = {
    "type": "Polygon",
    "coordinates": [
        [
            [0, 0],
            [0, 1],
            [1, 1],
            [1, 0],
            [0, 0],
        ],
        [
            [0, 0],
            [0.1, 0.9],
            [1, 1],
            [0.9, 0.1],
            [0, 0],
        ],
    ],
};
assert.writeError(t.insert({geo: polySharedVertices}));
