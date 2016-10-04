t = db.geo_s2index;
t.drop();

// We internally drop adjacent duplicate points in lines.
someline = {
    "type": "LineString",
    "coordinates": [[40, 5], [40, 5], [40, 5], [41, 6], [41, 6]]
};
t.insert({geo: someline, nonGeo: "someline"});
t.ensureIndex({geo: "2dsphere"});
foo = t.find({geo: {$geoIntersects: {$geometry: {type: "Point", coordinates: [40, 5]}}}}).next();
assert.eq(foo.geo, someline);
t.dropIndex({geo: "2dsphere"});

pointA = {
    "type": "Point",
    "coordinates": [40, 5]
};
t.insert({geo: pointA, nonGeo: "pointA"});

pointD = {
    "type": "Point",
    "coordinates": [41.001, 6.001]
};
t.insert({geo: pointD, nonGeo: "pointD"});

pointB = {
    "type": "Point",
    "coordinates": [41, 6]
};
t.insert({geo: pointB, nonGeo: "pointB"});

pointC = {
    "type": "Point",
    "coordinates": [41, 6]
};
t.insert({geo: pointC});

// Add a point within the polygon but not on the border.  Don't want to be on
// the path of the polyline.
pointE = {
    "type": "Point",
    "coordinates": [40.6, 5.4]
};
t.insert({geo: pointE});

// Make sure we can index this without error.
t.insert({nonGeo: "noGeoField!"});

somepoly = {
    "type": "Polygon",
    "coordinates": [[[40, 5], [40, 6], [41, 6], [41, 5], [40, 5]]]
};
t.insert({geo: somepoly, nonGeo: "somepoly"});

var res = t.ensureIndex({geo: "2dsphere", nonGeo: 1});
// We have a point without any geo data.  Don't error.
assert.commandWorked(res);

res = t.find({"geo": {"$geoIntersects": {"$geometry": pointA}}});
assert.eq(res.itcount(), 3);

res = t.find({"geo": {"$geoIntersects": {"$geometry": pointB}}});
assert.eq(res.itcount(), 4);

res = t.find({"geo": {"$geoIntersects": {"$geometry": pointD}}});
assert.eq(res.itcount(), 1);

res = t.find({"geo": {"$geoIntersects": {"$geometry": someline}}});
assert.eq(res.itcount(), 5);

res = t.find({"geo": {"$geoIntersects": {"$geometry": somepoly}}});
assert.eq(res.itcount(), 6);

res = t.find({"geo": {"$within": {"$geometry": somepoly}}});
assert.eq(res.itcount(), 6);

res = t.find({"geo": {"$geoIntersects": {"$geometry": somepoly}}}).limit(1);
assert.eq(res.itcount(), 1);

res = t.find({"nonGeo": "pointA", "geo": {"$geoIntersects": {"$geometry": somepoly}}});
assert.eq(res.itcount(), 1);

// Don't crash mongod if we give it bad input.
t.drop();
t.ensureIndex({loc: "2dsphere", x: 1});
t.save({loc: [0, 0]});
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
t.ensureIndex({loc: "2dsphere"});
res = t.insert({
    loc: {
        type: 'Point',
        coordinates: [40, 5],
        crs: {type: 'name', properties: {name: 'EPSG:2000'}}
    }
});
assert.writeError(res);
assert.eq(0, t.find().itcount());
res = t.insert({loc: {type: 'Point', coordinates: [40, 5]}});
assert.writeOK(res);
res = t.insert({
    loc: {
        type: 'Point',
        coordinates: [40, 5],
        crs: {type: 'name', properties: {name: 'EPSG:4326'}}
    }
});
assert.writeOK(res);
res = t.insert({
    loc: {
        type: 'Point',
        coordinates: [40, 5],
        crs: {type: 'name', properties: {name: 'urn:ogc:def:crs:OGC:1.3:CRS84'}}
    }
});
assert.writeOK(res);

// We can pass level parameters and we verify that they're valid.
// 0 <= coarsestIndexedLevel <= finestIndexedLevel <= 30.
t.drop();
t.save({loc: [0, 0]});
res = t.ensureIndex({loc: "2dsphere"}, {finestIndexedLevel: 17, coarsestIndexedLevel: 5});
assert.commandWorked(res);
// Ensure the index actually works at a basic level
assert.neq(null, t.findOne({loc: {$geoNear: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}));

t.drop();
t.save({loc: [0, 0]});
res = t.ensureIndex({loc: "2dsphere"}, {finestIndexedLevel: 31, coarsestIndexedLevel: 5});
assert.commandFailed(res);

t.drop();
t.save({loc: [0, 0]});
res = t.ensureIndex({loc: "2dsphere"}, {finestIndexedLevel: 30, coarsestIndexedLevel: 0});
assert.commandWorked(res);
// Ensure the index actually works at a basic level
assert.neq(null, t.findOne({loc: {$geoNear: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}));

t.drop();
t.save({loc: [0, 0]});
res = t.ensureIndex({loc: "2dsphere"}, {finestIndexedLevel: 30, coarsestIndexedLevel: -1});
assert.commandFailed(res);

// SERVER-21491 Verify that 2dsphere index options require correct types.
res = t.ensureIndex({loc: '2dsphere'}, {'2dsphereIndexVersion': 'NOT_A_NUMBER'});
assert.commandFailed(res);

res = t.ensureIndex({loc: '2dsphere'}, {finestIndexedLevel: 'NOT_A_NUMBER'});
assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch);

res = t.ensureIndex({loc: '2dsphere'}, {coarsestIndexedLevel: 'NOT_A_NUMBER'});
assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch);

// Ensure polygon which previously triggered an assertion error in SERVER-19674
// is able to be indexed.
t.drop();
t.insert({
    loc: {
        "type": "Polygon",
        "coordinates": [[[-45, 0], [-44.875, 0], [-44.875, 0.125], [-45, 0.125], [-45, 0]]]
    }
});
res = t.createIndex({loc: "2dsphere"});
assert.commandWorked(res);