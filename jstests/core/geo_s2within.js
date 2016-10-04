// Test some cases that might be iffy with $within, mostly related to polygon w/holes.
t = db.geo_s2within;
t.drop();
t.ensureIndex({geo: "2dsphere"});

somepoly = {
    "type": "Polygon",
    "coordinates": [[[40, 5], [40, 6], [41, 6], [41, 5], [40, 5]]]
};

t.insert({geo: {"type": "LineString", "coordinates": [[40.1, 5.1], [40.2, 5.2]]}});
// This is only partially contained within the polygon.
t.insert({geo: {"type": "LineString", "coordinates": [[40.1, 5.1], [42, 7]]}});

res = t.find({"geo": {"$within": {"$geometry": somepoly}}});
assert.eq(res.itcount(), 1);

t.drop();
t.ensureIndex({geo: "2dsphere"});
somepoly = {
    "type": "Polygon",
    "coordinates": [
        [[40, 5], [40, 8], [43, 8], [43, 5], [40, 5]],
        [[41, 6], [42, 6], [42, 7], [41, 7], [41, 6]]
    ]
};

t.insert({geo: {"type": "Point", "coordinates": [40, 5]}});
res = t.find({"geo": {"$within": {"$geometry": somepoly}}});
assert.eq(res.itcount(), 1);
// In the hole.  Shouldn't find it.
t.insert({geo: {"type": "Point", "coordinates": [41.1, 6.1]}});
res = t.find({"geo": {"$within": {"$geometry": somepoly}}});
assert.eq(res.itcount(), 1);
// Also in the hole.
t.insert({geo: {"type": "LineString", "coordinates": [[41.1, 6.1], [41.2, 6.2]]}});
res = t.find({"geo": {"$within": {"$geometry": somepoly}}});
assert.eq(res.itcount(), 1);
// Half-hole, half-not.  Shouldn't be $within.
t.insert({geo: {"type": "LineString", "coordinates": [[41.5, 6.5], [42.5, 7.5]]}});
res = t.find({"geo": {"$within": {"$geometry": somepoly}}});
assert.eq(res.itcount(), 1);
