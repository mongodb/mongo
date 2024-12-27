// @tags: [
//   requires_getmore,
// ]

const t = db.geo_s2index;
t.drop();

// Helper used to print debug info if any assertion fails.
function dumpCollection() {
    return "Collection contents: " + tojson(t.find().toArray());
}

// We internally drop adjacent duplicate points in lines.
const someline = {
    "type": "LineString",
    "coordinates": [[40, 5], [40, 5], [40, 5], [41, 6], [41, 6]]
};
assert.commandWorked(t.insert({geo: someline, nonGeo: "someline"}));
assert.commandWorked(t.createIndex({geo: "2dsphere"}));
const results =
    t.find({geo: {$geoIntersects: {$geometry: {type: "Point", coordinates: [40, 5]}}}}).toArray();
assert.eq(results.length, 1, dumpCollection);
assert.eq(results[0].geo, someline, dumpCollection);
t.dropIndex({geo: "2dsphere"});

const pointA = {
    "type": "Point",
    "coordinates": [40, 5]
};
t.insert({geo: pointA, nonGeo: "pointA"});

const pointD = {
    "type": "Point",
    "coordinates": [41.001, 6.001]
};
t.insert({geo: pointD, nonGeo: "pointD"});

const pointB = {
    "type": "Point",
    "coordinates": [41, 6]
};
t.insert({geo: pointB, nonGeo: "pointB"});

const pointC = {
    "type": "Point",
    "coordinates": [41, 6]
};
t.insert({geo: pointC});

// Add a point within the polygon but not on the border.  Don't want to be on
// the path of the polyline.
const pointE = {
    "type": "Point",
    "coordinates": [40.6, 5.4]
};
t.insert({geo: pointE});

// Make sure we can index this without error.
t.insert({nonGeo: "noGeoField!"});

const somepoly = {
    "type": "Polygon",
    "coordinates": [[[40, 5], [40, 6], [41, 6], [41, 5], [40, 5]]]
};
t.insert({geo: somepoly, nonGeo: "somepoly"});

{
    const res = t.createIndex({geo: "2dsphere", nonGeo: 1});
    // We have a point without any geo data.  Don't error.
    assert.commandWorked(res);
}

{
    const res = t.find({"geo": {"$geoIntersects": {"$geometry": pointA}}});
    assert.eq(res.itcount(), 3, dumpCollection);
}

{
    const res = t.find({"geo": {"$geoIntersects": {"$geometry": pointB}}});
    assert.eq(res.itcount(), 4, dumpCollection);
}

{
    const res = t.find({"geo": {"$geoIntersects": {"$geometry": pointD}}});
    assert.eq(res.itcount(), 1, dumpCollection);
}

{
    const res = t.find({"geo": {"$geoIntersects": {"$geometry": someline}}});
    assert.eq(res.itcount(), 5, dumpCollection);
}

{
    const res = t.find({"geo": {"$geoIntersects": {"$geometry": somepoly}}});
    assert.eq(res.itcount(), 6, dumpCollection);
}

{
    const res = t.find({"geo": {"$within": {"$geometry": somepoly}}});
    assert.eq(res.itcount(), 6, dumpCollection);
}

{
    const res = t.find({"geo": {"$geoIntersects": {"$geometry": somepoly}}}).limit(1);
    assert.eq(res.itcount(), 1, dumpCollection);
}

{
    const res = t.find({"nonGeo": "pointA", "geo": {"$geoIntersects": {"$geometry": somepoly}}});
    assert.eq(res.itcount(), 1, dumpCollection);
}

// Don't crash mongod if we give it bad input.
t.drop();
t.createIndex({loc: "2dsphere", x: 1});
assert.commandWorked(t.insert({loc: [0, 0]}));
assert.throws(function() {
    return t.count({loc: {$foo: [0, 0]}});
});
assert.throws(function() {
    return t
        .find({
            "nonGeo": "pointA",
            "geo": {"$geoIntersects": {"$geometry": somepoly}, "$near": {"$geometry": somepoly}}
        })
        .count();
});

// If we specify a datum, it has to be valid (WGS84).
t.drop();
t.createIndex({loc: "2dsphere"});
const failedInsertRes = t.insert({
    loc: {type: 'Point', coordinates: [40, 5], crs: {type: 'name', properties: {name: 'EPSG:2000'}}}
});
assert.writeError(failedInsertRes);
assert.eq(0, t.find().itcount(), dumpCollection);

assert.commandWorked(t.insert({loc: {type: 'Point', coordinates: [40, 5]}}));
assert.commandWorked(t.insert({
    loc: {type: 'Point', coordinates: [40, 5], crs: {type: 'name', properties: {name: 'EPSG:4326'}}}
}));
assert.commandWorked(t.insert({
    loc: {
        type: 'Point',
        coordinates: [40, 5],
        crs: {type: 'name', properties: {name: 'urn:ogc:def:crs:OGC:1.3:CRS84'}}
    }
}));

// We can pass level parameters and we verify that they're valid.
// 0 <= coarsestIndexedLevel <= finestIndexedLevel <= 30.
t.drop();
assert.commandWorked(t.insert({loc: [0, 0]}));
assert.commandWorked(
    t.createIndex({loc: "2dsphere"}, {finestIndexedLevel: 17, coarsestIndexedLevel: 5}));
// Ensure the index actually works at a basic level
assert.neq(null,
           t.findOne({loc: {$geoNear: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}),
           dumpCollection);

t.drop();
assert.commandWorked(t.insert({loc: [0, 0]}));
assert.commandFailed(
    t.createIndex({loc: "2dsphere"}, {finestIndexedLevel: 31, coarsestIndexedLevel: 5}));

t.drop();
assert.commandWorked(t.insert({loc: [0, 0]}));
assert.commandWorked(
    t.createIndex({loc: "2dsphere"}, {finestIndexedLevel: 30, coarsestIndexedLevel: 0}));

// Ensure the index actually works at a basic level
assert.neq(null,
           t.findOne({loc: {$geoNear: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}),
           dumpCollection);

{
    const created = t.getIndexes().filter((idx) => idx.hasOwnProperty("2dsphereIndexVersion"))[0];
    assert.eq(created.finestIndexedLevel, 30, created, dumpCollection);
    assert.eq(created.coarsestIndexedLevel, 0, created, dumpCollection);
}

t.drop();
assert.commandWorked(t.insert({loc: [0, 0]}));
assert.commandFailed(
    t.createIndex({loc: "2dsphere"}, {finestIndexedLevel: 30, coarsestIndexedLevel: -1}));

// SERVER-21491 Verify that 2dsphere index options require correct types.
assert.commandFailedWithCode(
    t.createIndex({loc: '2dsphere'}, {'2dsphereIndexVersion': 'NOT_A_NUMBER'}),
    ErrorCodes.TypeMismatch);

assert.commandFailedWithCode(t.createIndex({loc: '2dsphere'}, {finestIndexedLevel: 'NOT_A_NUMBER'}),
                             ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(
    t.createIndex({loc: '2dsphere'}, {coarsestIndexedLevel: 'NOT_A_NUMBER'}),
    ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(t.createIndex({loc: '2dsphere'}, {finestIndexedLevel: true}),
                             ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(t.createIndex({loc: '2dsphere'}, {coarsestIndexedLevel: true}),
                             ErrorCodes.TypeMismatch);

// Ensure polygon which previously triggered an assertion error in SERVER-19674
// is able to be indexed.
t.drop();
assert.commandWorked(t.insert({
    loc: {
        "type": "Polygon",
        "coordinates": [[[-45, 0], [-44.875, 0], [-44.875, 0.125], [-45, 0.125], [-45, 0]]]
    }
}));
assert.commandWorked(t.createIndex({loc: "2dsphere"}));
