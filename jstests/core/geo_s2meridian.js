t = db.geo_s2meridian;
t.drop();
t.ensureIndex({geo: "2dsphere"});

/*
 * Test 1: check that intersection works on the meridian.  We insert a line
 * that crosses the meridian, and then run a geoIntersect with a line
 * that runs along the meridian.
 */

meridianCrossingLine = {
    geo: {type: "LineString", coordinates: [[-178.0, 10.0], [178.0, 10.0]]}
};

assert.writeOK(t.insert(meridianCrossingLine));

lineAlongMeridian = {
    type: "LineString",
    coordinates: [[180.0, 11.0], [180.0, 9.0]]
};

result = t.find({geo: {$geoIntersects: {$geometry: lineAlongMeridian}}});
assert.eq(result.itcount(), 1);

t.drop();
t.ensureIndex({geo: "2dsphere"});
/*
 * Test 2: check that within work across the meridian.  We insert points
 * on the meridian, and immediately on either side, and confirm that a poly
 * covering all of them returns them all.
 */
pointOnNegativeSideOfMeridian = {
    geo: {type: "Point", coordinates: [-179.0, 1.0]}
};
pointOnMeridian = {
    geo: {type: "Point", coordinates: [180.0, 1.0]}
};
pointOnPositiveSideOfMeridian = {
    geo: {type: "Point", coordinates: [179.0, 1.0]}
};

t.insert(pointOnMeridian);
t.insert(pointOnNegativeSideOfMeridian);
t.insert(pointOnPositiveSideOfMeridian);

meridianCrossingPoly = {
    type: "Polygon",
    coordinates:
        [[[-178.0, 10.0], [178.0, 10.0], [178.0, -10.0], [-178.0, -10.0], [-178.0, 10.0]]]
};

result = t.find({geo: {$geoWithin: {$geometry: meridianCrossingPoly}}});
assert.eq(result.itcount(), 3);

t.drop();
t.ensureIndex({geo: "2dsphere"});
/*
 * Test 3: Check that near works around the meridian.  Insert two points, one
 * closer, but across the meridian, and confirm they both come back, and
 * that the order is correct.
 */
pointOnNegativeSideOfMerid = {
    name: "closer",
    geo: {type: "Point", coordinates: [-179.0, 0.0]}
};

pointOnPositiveSideOfMerid = {
    name: "farther",
    geo: {type: "Point", coordinates: [176.0, 0.0]}
};

t.insert(pointOnNegativeSideOfMerid);
t.insert(pointOnPositiveSideOfMerid);

pointOnPositiveSideOfMeridian = {
    type: "Point",
    coordinates: [179.0, 0.0]
};

result = t.find({geo: {$geoNear: pointOnPositiveSideOfMeridian}});
assert.eq(result.itcount(), 2);
result = t.find({geo: {$geoNear: pointOnPositiveSideOfMeridian}});
assert.eq(result[0].name, "closer");
assert.eq(result[1].name, "farther");
