// Test $geoNear + $within.
(function() {
    t = db.geo_s2nearwithin;
    t.drop();

    points = 10;
    for (var x = -points; x < points; x += 1) {
        for (var y = -points; y < points; y += 1) {
            assert.commandWorked(t.insert({geo: [x, y]}));
        }
    }

    origin = {"type": "Point", "coordinates": [0, 0]};

    assert.commandWorked(t.ensureIndex({geo: "2dsphere"}));
    // Near requires an index, and 2dsphere is an index.  Spherical isn't
    // specified so this doesn't work.
    let res = assert.commandFailedWithCode(t.runCommand("aggregate", {
        cursor: {},
        pipeline: [{
            $geoNear: {
                near: [0, 0],
                distanceField: "d",
                query: {geo: {$within: {$center: [[0, 0], 1]}}}
            }
        }],
    }),
                                           ErrorCodes.BadValue);
    assert(res.errmsg.includes("unable to find index for $geoNear query"), tojson(res));

    // Spherical is specified so this does work.  Old style points are weird
    // because you can use them with both $center and $centerSphere.  Points are
    // the only things we will do this conversion for.
    const runGeoNear = (within) => t.aggregate({
                                        $geoNear: {
                                            near: [0, 0],
                                            distanceField: "d",
                                            spherical: true,
                                            query: {geo: {$within: within}},
                                        }
                                    }).toArray();

    resNear = runGeoNear({$center: [[0, 0], 1]});
    assert.eq(resNear.length, 5);

    resNear = runGeoNear({$centerSphere: [[0, 0], Math.PI / 180.0]});
    assert.eq(resNear.length, 5);

    resNear = runGeoNear({$centerSphere: [[0, 0], 0]});
    assert.eq(resNear.length, 1);

    resNear = runGeoNear({$centerSphere: [[1, 0], 0.5 * Math.PI / 180.0]});
    assert.eq(resNear.length, 1);

    resNear = runGeoNear({$center: [[1, 0], 1.5]});
    assert.eq(resNear.length, 9);
}());
