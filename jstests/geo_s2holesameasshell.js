t = db.geo_s2holessameasshell
t.drop();
t.ensureIndex({geo: "2dsphere"});

centerPoint = {"type": "Point", "coordinates": [0.5, 0.5]};
edgePoint = {"type": "Point", "coordinates": [0, 0.5]};
cornerPoint = {"type": "Point", "coordinates": [0, 0]};

// Various "edge" cases.  None of them should be returned by the non-polygon
// polygon below.
t.insert({geo : centerPoint});
t.insert({geo : edgePoint});
t.insert({geo : cornerPoint});

// This generates an empty covering.
polygonWithFullHole = { "type" : "Polygon", "coordinates": [
        [[0,0], [0,1], [1, 1], [1, 0], [0, 0]],
        [[0,0], [0,1], [1, 1], [1, 0], [0, 0]]
    ]
};

// No keys for insert should error.
t.insert({geo: polygonWithFullHole})
assert(db.getLastError())

// No covering to search over should give an empty result set.
res = t.find({geo: {$within: {$geometry: polygonWithFullHole}}});
assert.eq(res.count(), 0)
