var t = db.geo_s2overlappingpolys;
t.drop();

t.ensureIndex({geo: "2dsphere"});

var minError = 0.8e-13;

var canonPoly = {
    type: "Polygon",
    coordinates: [[[-1.0, -1.0], [1.0, -1.0], [1.0, 1.0], [-1.0, 1.0], [-1.0, -1.0]]]
};
t.insert({geo: canonPoly});

// Test 1: If a poly completely encloses the canonPoly, we expect the canonPoly
// to be returned for both $within and $geoIntersect

var outerPoly = {
    type: "Polygon",
    coordinates: [[[-2.0, -2.0], [2.0, -2.0], [2.0, 2.0], [-2.0, 2.0], [-2.0, -2.0]]]
};
var result = t.find({geo: {$within: {$geometry: outerPoly}}});
assert.eq(result.itcount(), 1);
result = t.find({geo: {$geoIntersects: {$geometry: outerPoly}}});
assert.eq(result.itcount(), 1);

// Test 2: If a poly that covers half of the canonPoly, we expect that it should
// geoIntersect, but should not be within.

var partialPoly = {
    type: "Polygon",
    coordinates: [[[-2.0, -2.0], [2.0, -2.0], [2.0, 0.0], [-2.0, 0.0], [-2.0, -2.0]]]
};

// Should not be within
result = t.find({geo: {$within: {$geometry: partialPoly}}});
assert.eq(result.itcount(), 0);

// This should however count as a geoIntersect
result = t.find({geo: {$geoIntersects: {$geometry: partialPoly}}});
assert.eq(result.itcount(), 1);

// Test 3: Polygons that intersect at a point or an edge have undefined
// behaviour in s2 The s2 library we're using appears to have
// the following behaviour.

// Case (a): Polygons that intersect at one point (not a vertex).
// behaviour: geoIntersects.

var sharedPointPoly = {
    type: "Polygon",
    coordinates: [[[0.0, -2.0], [0.0, -1.0], [1.0, -2.0], [0.0, -2.0]]]
};

result = t.find({geo: {$geoIntersects: {$geometry: sharedPointPoly}}});
assert.eq(result.itcount(), 1);

// Case (b): Polygons that intersect at one point (a vertex).
// behaviour: not geoIntersect

var sharedVertexPoly = {
    type: "Polygon",
    coordinates: [[[0.0, -2.0], [1.0, -1.0], [1.0, -2.0], [0.0, -2.0]]]
};

result = t.find({geo: {$geoIntersects: {$geometry: sharedVertexPoly}}});
assert.eq(result.itcount(), 0);

// Case (c): Polygons that intesersect at one point that is very close to a
// vertex should have the same behaviour as Case (b).

var almostSharedVertexPoly = {
    type: "Polygon",
    coordinates: [[[0.0, -2.0], [1.0 - minError, -1.0], [1.0, -2.0], [0.0, -2.0]]]
};

result = t.find({geo: {$geoIntersects: {$geometry: almostSharedVertexPoly}}});
assert.eq(result.itcount(), 0);

// Case (d): Polygons that intesersect at one point that is not quite as close
// to a vertex should behave as though it were not a vertex, and should
// geoIntersect

var notCloseEnoughSharedVertexPoly = {
    type: "Polygon",
    coordinates: [[[0.0, -2.0], [1.0 - (10 * minError), -1.0], [1.0, -2.0], [0.0, -2.0]]]
};

result = t.find({geo: {$geoIntersects: {$geometry: notCloseEnoughSharedVertexPoly}}});
assert.eq(result.itcount(), 1);

// Case (e): Polygons that come very close to having a point intersection
// on a non-vertex coordinate should intersect.

var almostSharedPointPoly = {
    type: "Polygon",
    coordinates: [[[0.0, -2.0], [0.0, (-1.0 - minError)], [1.0, -2.0], [0.0, -2.0]]]
};

result = t.find({geo: {$geoIntersects: {$geometry: almostSharedPointPoly}}});
assert.eq(result.itcount(), 1);

// Case (f): If we increase the error a little, it should no longer act
// as though it's intersecting.
// NOTE: I think this error bound seems odd. Going to 0.000152297 will break this test.
// I've confirmed there is an error bound, but it's a lot larger than we experienced above.
var errorBound = 0.000152298;
var notCloseEnoughSharedPointPoly = {
    type: "Polygon",
    coordinates: [[[0.0, -2.0], [0.0, -1.0 - errorBound], [1.0, -2.0], [0.0, -2.0]]]
};

result = t.find({geo: {$geoIntersects: {$geometry: notCloseEnoughSharedPointPoly}}});
assert.eq(result.itcount(), 0);

/* Test 3: Importantly, polygons with shared edges have undefined intersection
 * under s2. Therefore these test serve more to make sure nothing changes than
 * to confirm an expected behaviour.
 */

// Case 1: A polygon who shares an edge with another polygon, where the searching
//         polygon's edge is fully covered by the canon polygon's edge.
// Result: No intersection.
var fullyCoveredEdgePoly = {
    type: "Polygon",
    coordinates: [[[-2.0, -0.5], [-1.0, -0.5], [-1.0, 0.5], [-2.0, 0.5], [-2.0, -0.5]]]
};

result = t.find({geo: {$geoIntersects: {$geometry: fullyCoveredEdgePoly}}});
assert.eq(result.itcount(), 0);

// Case 2: A polygon who shares an edge with another polygon, where the searching
//         polygon's edge fully covers the canon polygon's edge.
// Result: Intersection.
var coveringEdgePoly = {
    type: "Polygon",
    coordinates: [[[-2.0, -1.5], [-1.0, -1.5], [-1.0, 1.5], [-2.0, 1.5], [-2.0, -1.5]]]
};

result = t.find({geo: {$geoIntersects: {$geometry: coveringEdgePoly}}});
assert.eq(result.itcount(), 1);

// Case 2a: same as Case 2, except pulled slightly away from the polygon.
// Result: Intersection.
// NOTE: Scales of errors?
var closebyCoveringEdgePoly = {
    type: "Polygon",
    coordinates: [[
        [-2.0, -1.5],
        [-1.0 - (minError / 1000), -1.5],
        [-1.0 - (minError / 1000), 1.5],
        [-2.0, 1.5],
        [-2.0, -1.5]
    ]]
};

result = t.find({geo: {$geoIntersects: {$geometry: closebyCoveringEdgePoly}}});
assert.eq(result.itcount(), 1);

// Case 2b: same as Case 4, except pulled slightly away from the polygon, so that it's not
// intersecting.
// Result: No Intersection.
// NOTE: Scales of errors?
var notCloseEnoughCoveringEdgePoly = {
    type: "Polygon",
    coordinates: [[
        [-2.0, -1.5],
        [-1.0 - (minError / 100), -1.5],
        [-1.0 - (minError / 100), 1.5],
        [-2.0, 1.5],
        [-2.0, -1.5]
    ]]
};

result = t.find({geo: {$geoIntersects: {$geometry: notCloseEnoughCoveringEdgePoly}}});
assert.eq(result.itcount(), 0);

// Case 3: A polygon who shares an edge with another polygon, where the searching
//         polygon's edge partially covers by the canon polygon's edge.
// Result: No intersection.
var partiallyCoveringEdgePoly = {
    type: "Polygon",
    coordinates: [[[-2.0, -1.5], [-1.0, -1.5], [-1.0, 0.5], [-2.0, 0.5], [-2.0, -1.5]]]
};

result = t.find({geo: {$geoIntersects: {$geometry: partiallyCoveringEdgePoly}}});
assert.eq(result.itcount(), 0);

// Polygons that intersect at three non-co-linear points should geoIntersect
var sharedPointsPoly = {
    type: "Polygon",
    coordinates: [[
        [0.0, -3.0],
        [0.0, -1.0],
        [2.0, -2.0],
        [1.0, 0.0],
        [2.0, 2.0],
        [0.0, 1.0],
        [0.0, 3.0],
        [3.0, 3.0],
        [3.0, -3.0],
        [0.0, -3.0]
    ]]
};

result = t.find({geo: {$geoIntersects: {$geometry: sharedPointsPoly}}});
assert.eq(result.itcount(), 1);

// If a polygon contains a hole, and another polygon is within that hole, it should not be within or
// intersect.

var bigHolePoly = {
    type: "Polygon",
    coordinates: [
        [[-3.0, -3.0], [3.0, -3.0], [3.0, 3.0], [-3.0, 3.0], [-3.0, -3.0]],
        [[-2.0, -2.0], [2.0, -2.0], [2.0, 2.0], [-2.0, 2.0], [-2.0, -2.0]]
    ]
};
result = t.find({geo: {$within: {$geometry: bigHolePoly}}});
assert.eq(result.itcount(), 0);
result = t.find({geo: {$geoIntersects: {$geometry: bigHolePoly}}});
assert.eq(result.itcount(), 0);

// If a polygon has a hole, and another polygon is contained partially by that hole, it should be an
// intersection
// but not a within.

var internalOverlapPoly = {
    type: "Polygon",
    coordinates: [
        [[-3.0, -3.0], [3.0, -3.0], [3.0, 3.0], [-3.0, 3.0], [-3.0, -3.0]],
        [[-2.0, 0.0], [2.0, 0.0], [2.0, 2.0], [-2.0, 2.0], [-2.0, 0.0]]
    ]
};

result = t.find({geo: {$geoIntersects: {$geometry: internalOverlapPoly}}});
assert.eq(result.itcount(), 1);
result = t.find({geo: {$within: {$geometry: internalOverlapPoly}}});
assert.eq(result.itcount(), 0);
