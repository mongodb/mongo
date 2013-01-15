t = db.geo_s2oddshapes
t.drop()
t.ensureIndex( { geo : "2dsphere" } );

testPoint = {
    name: "origin",
    geo: {
        type: "Point",
        coordinates: [0.0, 0.0]
    }
};

testHorizLine = {
    name: "horiz",
    geo: {
        type: "LineString",
        coordinates: [[-2.0, 10.0], [2.0, 10.0]]
    }
};

testVertLine = {
    name: "vert",
    geo: {
        type: "LineString",
        coordinates: [[10.0, -2.0], [10.0, 2.0]]
    }
};

t.insert(testPoint);
t.insert(testHorizLine);
t.insert(testVertLine);

//Test a poly that runs vertically all the way along the meridian.

tallPoly = {type: "Polygon",
    coordinates: [
        [[1.0, 89.0], [-1.0, 89.0], [-1.0, -89.0], [1.0, -89.0], [1.0, 89.0]]
    ]};
//We expect that the testPoint (at the origin) will be within this poly.
result = t.find({geo: {$within: {$geometry: tallPoly}}});
assert.eq(result.count(), 1);
assert.eq(result[0].name, 'origin');

//We expect that the testPoint, and the testHorizLine should geoIntersect
//with this poly.
result = t.find({geo: {$geoIntersects: {$geometry: tallPoly}}});
assert.eq(result.count(), 2);
assert.eq(result[0].name, 'horiz');
assert.eq(result[1].name, 'origin');

//Test a poly that runs horizontally along the equator.

longPoly = {type: "Polygon",
    coordinates: [
        [[89.0, 1.0], [-89.0, 1.0], [-89.0, -1.0], [89.0, -1.0], [89.0, 1.0]]
    ]};

//We expect that the testPoint (at the origin) will be within this poly.
result = t.find({geo: {$within: {$geometry: longPoly}}});
assert.eq(result.count(), 1);
assert.eq(result[0].name, 'origin');

//We expect that the testPoint, and the testVertLine should geoIntersect
//with this poly.
result = t.find({geo: {$geoIntersects: {$geometry: longPoly}}});
assert.eq(result.count(), 2);
assert.eq(result[0].name, 'vert');
assert.eq(result[1].name, 'origin');

//Test a poly that is the size of half the earth.

t.drop()
t.ensureIndex( { geo : "2dsphere" } );

insidePoint = {
    name: "inside",
    geo: {
        type: "Point",
        name: "inside",
        coordinates: [100.0, 0.0]
    }
};

outsidePoint = {
    name: "inside",
    geo: {
        type: "Point",
        name: "inside",
        coordinates: [-100.0, 0.0]
    }
};

t.insert(insidePoint);
t.insert(outsidePoint);

largePoly = {type: "Polygon",
    coordinates: [
        [[0.0, -90.0], [0.0, 90.0], [180.0, 0], [0.0, -90.0]]
    ]};

result = t.find({geo: {$within: {$geometry: largePoly}}});
assert.eq(result.count(), 1);
point = result[0]
assert.eq(point.name, 'inside');

//Test a poly that is very small.  A couple meters around.

t.drop()
t.ensureIndex( { geo : "2dsphere" } );

insidePoint = {
    name: "inside",
    geo: {
        type: "Point",
        name: "inside",
        coordinates: [0.01, 0.0]
    }};

outsidePoint = {
    name: "inside",
    geo: {
        type: "Point",
        name: "inside",
        coordinates: [0.2, 0.0]
    }};

t.insert(insidePoint);
t.insert(outsidePoint);

smallPoly = {type: "Polygon",
    coordinates: [
        [[0.0, -0.01], [0.015, -0.01], [0.015, 0.01], [0.0, 0.01], [0.0, -0.01]]
    ]};

result = t.find({geo: {$within: {$geometry: smallPoly}}});
assert.eq(result.count(), 1);
assert.eq(point.name, 'inside');

