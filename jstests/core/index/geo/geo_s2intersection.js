/**
 * Tests $geoIntersect basic functionality.
 */
let t = db.geo_s2intersectinglines;
t.drop();
t.createIndex({geo: "2dsphere"});

/**All the tests in this file are generally confirming intersections based upon
 * these three geo objects.
 */
let canonLine = {
    name: "canonLine",
    geo: {
        type: "LineString",
        coordinates: [
            [0.0, 0.0],
            [1.0, 0.0],
        ],
    },
};

let canonPoint = {name: "canonPoint", geo: {type: "Point", coordinates: [10.0, 10.0]}};

let canonPoly = {
    name: "canonPoly",
    geo: {
        type: "Polygon",
        coordinates: [
            [
                [50.0, 50.0],
                [51.0, 50.0],
                [51.0, 51.0],
                [50.0, 51.0],
                [50.0, 50.0],
            ],
        ],
    },
};

t.insert(canonLine);
t.insert(canonPoint);
t.insert(canonPoly);

// Case 1: Basic sanity intersection.
let testLine = {
    type: "LineString",
    coordinates: [
        [0.5, 0.5],
        [0.5, -0.5],
    ],
};

var result = t.find({geo: {$geoIntersects: {$geometry: testLine}}});
assert.eq(result.count(), 1);
assert.eq(result[0]["name"], "canonLine");

// Case 2: Basic Polygon intersection.
// we expect that the canonLine should intersect with this polygon.
let testPoly = {
    type: "Polygon",
    coordinates: [
        [
            [0.4, -0.1],
            [0.4, 0.1],
            [0.6, 0.1],
            [0.6, -0.1],
            [0.4, -0.1],
        ],
    ],
};

result = t.find({geo: {$geoIntersects: {$geometry: testPoly}}});
assert.eq(result.count(), 1);
assert.eq(result[0]["name"], "canonLine");

// Case 3: Intersects the vertex of a line.
// When a line intersects the vertex of a line, we expect this to
// count as a geoIntersection.
testLine = {
    type: "LineString",
    coordinates: [
        [0.0, 0.5],
        [0.0, -0.5],
    ],
};

result = t.find({geo: {$geoIntersects: {$geometry: testLine}}});
assert.eq(result.count(), 1);
assert.eq(result[0]["name"], "canonLine");

// Case 4: Sanity no intersection.
// This line just misses the canonLine in the negative direction.  This
// should not count as a geoIntersection.
testLine = {
    type: "LineString",
    coordinates: [
        [-0.1, 0.5],
        [-0.1, -0.5],
    ],
};

result = t.find({geo: {$geoIntersects: {$geometry: testLine}}});
assert.eq(result.count(), 0);

// Case 5: Overlapping line - only partially overlaps.
// Undefined behaviour: does intersect
testLine = {
    type: "LineString",
    coordinates: [
        [-0.5, 0.0],
        [0.5, 0.0],
    ],
};

var result = t.find({geo: {$geoIntersects: {$geometry: testLine}}});
assert.eq(result.count(), 1);
assert.eq(result[0]["name"], "canonLine");

// Case 6: Contained line - this line is fully contained by the canonLine
// Undefined behaviour: doesn't intersect.
testLine = {
    type: "LineString",
    coordinates: [
        [0.1, 0.0],
        [0.9, 0.0],
    ],
};

result = t.find({geo: {$geoIntersects: {$geometry: testLine}}});
assert.eq(result.count(), 0);

// Case 7: Identical line in the identical position.
// Undefined behaviour: does intersect.
testLine = {
    type: "LineString",
    coordinates: [
        [0.0, 0.0],
        [1.0, 0.0],
    ],
};

result = t.find({geo: {$geoIntersects: {$geometry: testLine}}});
assert.eq(result.count(), 1);
assert.eq(result[0]["name"], "canonLine");

// Case 8: Point intersection - we search with a line that intersects
// with the canonPoint.
testLine = {
    type: "LineString",
    coordinates: [
        [10.0, 11.0],
        [10.0, 9.0],
    ],
};

result = t.find({geo: {$geoIntersects: {$geometry: testLine}}});
assert.eq(result.count(), 1);
assert.eq(result[0]["name"], "canonPoint");

// Case 9: Point point intersection
// as above but with an identical point to the canonPoint.  We expect an
// intersection here.
testPoint = {
    type: "Point",
    coordinates: [10.0, 10.0],
};

result = t.find({geo: {$geoIntersects: {$geometry: testPoint}}});
assert.eq(result.count(), 1);
assert.eq(result[0]["name"], "canonPoint");

// Case 10: Sanity point non-intersection.
var testPoint = {type: "Point", coordinates: [12.0, 12.0]};

result = t.find({geo: {$geoIntersects: {$geometry: testPoint}}});
assert.eq(result.count(), 0);

// Case 11: Point polygon intersection
// verify that a point inside a polygon $geoIntersects.
testPoint = {
    type: "Point",
    coordinates: [50.5, 50.5],
};

result = t.find({geo: {$geoIntersects: {$geometry: testPoint}}});
assert.eq(result.count(), 1);
assert.eq(result[0]["name"], "canonPoly");

// Case 12: Make sure that we properly handle and $and of two
// $geoIntersects predicates.
t.drop();
t.createIndex({a: "2dsphere"});
t.insert({
    a: {
        type: "Polygon",
        coordinates: [
            [
                [0, 0],
                [3, 6],
                [6, 0],
                [0, 0],
            ],
        ],
    },
});

let firstPoint = {$geometry: {type: "Point", coordinates: [3.0, 1.0]}};
let secondPoint = {$geometry: {type: "Point", coordinates: [4.0, 1.0]}};

// First point should intersect with the polygon.
result = t.find({a: {$geoIntersects: firstPoint}});
assert.eq(result.count(), 1);

// Second point also intersects with the polygon.
result = t.find({a: {$geoIntersects: secondPoint}});
assert.eq(result.count(), 1);

// Both points intersect with the polygon, so the $and of
// two $geoIntersects should as well.
result = t.find({$and: [{a: {$geoIntersects: firstPoint}}, {a: {$geoIntersects: secondPoint}}]});
assert.eq(result.count(), 1);
