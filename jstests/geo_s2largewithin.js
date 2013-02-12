// If our $within is enormous, create a coarse covering for the search so it
// doesn't take forever.
t = db.geo_s2largewithin
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

//Test a poly that runs horizontally along the equator.

longPoly = {type: "Polygon",
    coordinates: [
        [[30.0, 1.0], [-30.0, 1.0], [-30.0, -1.0], [30.0, -1.0], [30.0, 1.0]]
    ]};

result = t.find({geo: {$geoWithin: {$geometry: longPoly}}});
assert.eq(result.count(), 1);
assert.eq("origin", result[0].name)
