// Verify that odd polygons (huge or "narrow") behave as we expect.
// Note that since 2dsphere is spherical, polygons that seem narrow are actually
// rather wide if their latitude (or longitude) range is large.
// @tags: [
//   requires_getmore,
// ]

let t = db.geo_s2oddshapes;
t.drop();
t.createIndex({geo: "2dsphere"});

let testPoint = {name: "origin", geo: {type: "Point", coordinates: [0.0, 0.0]}};

let testHorizLine = {
    name: "horiz",
    geo: {
        type: "LineString",
        coordinates: [
            [-2.0, 10.0],
            [2.0, 10.0],
        ],
    },
};

let testVertLine = {
    name: "vert",
    geo: {
        type: "LineString",
        coordinates: [
            [10.0, -2.0],
            [10.0, 2.0],
        ],
    },
};

t.insert(testPoint);
t.insert(testHorizLine);
t.insert(testVertLine);

// Test a poly that runs vertically all the way along the meridian.

let tallPoly = {
    type: "Polygon",
    coordinates: [
        [
            [1.0, 89.0],
            [-1.0, 89.0],
            [-1.0, -89.0],
            [1.0, -89.0],
            [1.0, 89.0],
        ],
    ],
};
// We expect that the testPoint (at the origin) will be within this poly.
var result = t.find({geo: {$within: {$geometry: tallPoly}}});
assert.eq(result.itcount(), 1);
var result = t.find({geo: {$within: {$geometry: tallPoly}}});
assert.eq(result[0].name, "origin");

// We expect that the testPoint, and the testHorizLine should geoIntersect
// with this poly.
result = t.find({geo: {$geoIntersects: {$geometry: tallPoly}}});
assert.eq(result.itcount(), 2);
result = t.find({geo: {$geoIntersects: {$geometry: tallPoly}}});

// Test a poly that runs horizontally along the equator.

let longPoly = {
    type: "Polygon",
    coordinates: [
        [
            [89.0, 1.0],
            [-89.0, 1.0],
            [-89.0, -1.0],
            [89.0, -1.0],
            [89.0, 1.0],
        ],
    ],
};

// Thanks to spherical geometry, this poly contains most of the hemisphere.
result = t.find({geo: {$within: {$geometry: longPoly}}});
assert.eq(result.itcount(), 3);
result = t.find({geo: {$geoIntersects: {$geometry: longPoly}}});
assert.eq(result.itcount(), 3);

// Test a poly that is the size of half the earth.

t.drop();
t.createIndex({geo: "2dsphere"});

let insidePoint = {name: "inside", geo: {type: "Point", name: "inside", coordinates: [100.0, 0.0]}};

let outsidePoint = {
    name: "inside",
    geo: {type: "Point", name: "inside", coordinates: [-100.0, 0.0]},
};

t.insert(insidePoint);
t.insert(outsidePoint);

let largePoly = {
    type: "Polygon",
    coordinates: [
        [
            [0.0, -90.0],
            [0.0, 90.0],
            [180.0, 0],
            [0.0, -90.0],
        ],
    ],
};

result = t.find({geo: {$within: {$geometry: largePoly}}});
assert.eq(result.itcount(), 1);
result = t.find({geo: {$within: {$geometry: largePoly}}});
let point = result[0];
assert.eq(point.name, "inside");

// Test a poly that is very small.  A couple meters around.

t.drop();
t.createIndex({geo: "2dsphere"});

insidePoint = {
    name: "inside",
    geo: {type: "Point", name: "inside", coordinates: [0.01, 0.0]},
};

outsidePoint = {
    name: "inside",
    geo: {type: "Point", name: "inside", coordinates: [0.2, 0.0]},
};

t.insert(insidePoint);
t.insert(outsidePoint);

let smallPoly = {
    type: "Polygon",
    coordinates: [
        [
            [0.0, -0.01],
            [0.015, -0.01],
            [0.015, 0.01],
            [0.0, 0.01],
            [0.0, -0.01],
        ],
    ],
};

result = t.find({geo: {$within: {$geometry: smallPoly}}});
assert.eq(result.itcount(), 1);
result = t.find({geo: {$within: {$geometry: smallPoly}}});
point = result[0];
assert.eq(point.name, "inside");
