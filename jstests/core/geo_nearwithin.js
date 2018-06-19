// Test $near + $within.
(function() {
    t = db.geo_nearwithin;
    t.drop();

    points = 10;
    for (var x = -points; x < points; x += 1) {
        for (var y = -points; y < points; y += 1) {
            assert.commandWorked(t.insert({geo: [x, y]}));
        }
    }

    assert.commandWorked(t.ensureIndex({geo: "2d"}));

    const runQuery = (center) =>
        t.find({$and: [{geo: {$near: [0, 0]}}, {geo: {$within: {$center: center}}}]}).toArray();

    resNear = runQuery([[0, 0], 1]);
    assert.eq(resNear.length, 5);

    resNear = runQuery([[0, 0], 0]);
    assert.eq(resNear.length, 1);

    resNear = runQuery([[1, 0], 0.5]);
    assert.eq(resNear.length, 1);

    resNear = runQuery([[1, 0], 1.5]);
    assert.eq(resNear.length, 9);

    // We want everything distance >1 from us but <1.5
    // These points are (-+1, -+1)
    resNear = t.find({
                   $and: [
                       {geo: {$near: [0, 0]}},
                       {geo: {$within: {$center: [[0, 0], 1.5]}}},
                       {geo: {$not: {$within: {$center: [[0, 0], 1]}}}}
                   ]
               }).toArray();
    assert.eq(resNear.length, 4);
}());
