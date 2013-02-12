// Explode arrays when indexing non-geo fields in 2dsphere, and make sure that
// we find them with queries.
t = db.geo_s2nongeoarray

oldPoint = [40,5]

var data = {geo: oldPoint, nonGeo: [123,456], otherNonGeo: [{b:[1,2]},{b:[3,4]}]};

t.drop();
t.insert(data);
assert(!db.getLastError());
t.ensureIndex({otherNonGeo: 1});
assert(!db.getLastError());
assert.eq(1, t.find({otherNonGeo: {b:[1,2]}}).itcount());
assert.eq(0, t.find({otherNonGeo: 1}).itcount());
assert.eq(1, t.find({'otherNonGeo.b': 1}).itcount());

t.drop();
t.insert(data);
t.ensureIndex({geo: "2d", nonGeo: 1, otherNonGeo: 1})
assert.eq(t.find({nonGeo: 123, geo: {$nearSphere: oldPoint}}).itcount(), 1);
assert.eq(t.find({'otherNonGeo.b': 1, geo: {$nearSphere: oldPoint}}).itcount(), 1);

t.drop()
t.insert(data);
t.ensureIndex({geo: "2dsphere", nonGeo: 1, otherNonGeo: 1})
assert.eq(t.find({nonGeo: 123, geo: {$nearSphere: oldPoint}}).itcount(), 1);
assert.eq(t.find({'otherNonGeo.b': 1, geo: {$nearSphere: oldPoint}}).itcount(), 1);
