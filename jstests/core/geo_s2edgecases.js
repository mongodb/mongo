t = db.geo_s2edgecases;
t.drop();

roundworldpoint = {
    "type": "Point",
    "coordinates": [180, 0]
};

// Opposite the equator
roundworld = {
    "type": "Polygon",
    "coordinates": [[[179, 1], [-179, 1], [-179, -1], [179, -1], [179, 1]]]
};
t.insert({geo: roundworld});

roundworld2 = {
    "type": "Polygon",
    "coordinates": [[[179, 1], [179, -1], [-179, -1], [-179, 1], [179, 1]]]
};
t.insert({geo: roundworld2});

// North pole
santapoint = {
    "type": "Point",
    "coordinates": [180, 90]
};
santa = {
    "type": "Polygon",
    "coordinates": [[[179, 89], [179, 90], [-179, 90], [-179, 89], [179, 89]]]
};
t.insert({geo: santa});
santa2 = {
    "type": "Polygon",
    "coordinates": [[[179, 89], [-179, 89], [-179, 90], [179, 90], [179, 89]]]
};
t.insert({geo: santa2});

// South pole
penguinpoint = {
    "type": "Point",
    "coordinates": [0, -90]
};
penguin1 = {
    "type": "Polygon",
    "coordinates": [[[0, -89], [0, -90], [179, -90], [179, -89], [0, -89]]]
};
t.insert({geo: penguin1});
penguin2 = {
    "type": "Polygon",
    "coordinates": [[[0, -89], [179, -89], [179, -90], [0, -90], [0, -89]]]
};
t.insert({geo: penguin2});

t.ensureIndex({geo: "2dsphere", nonGeo: 1});

res = t.find({"geo": {"$geoIntersects": {"$geometry": roundworldpoint}}});
assert.eq(res.count(), 2);
res = t.find({"geo": {"$geoIntersects": {"$geometry": santapoint}}});
assert.eq(res.count(), 2);
res = t.find({"geo": {"$geoIntersects": {"$geometry": penguinpoint}}});
assert.eq(res.count(), 2);
