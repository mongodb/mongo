// Explode arrays when indexing non-geo fields in 2dsphere, and make sure that
// we find them with queries.
t = db.geo_s2nongeoarray

oldPoint = [40,5]

t.insert({geo: oldPoint, nonGeo: [123,456]});
assert.eq(t.find({nonGeo: 123 }).itcount(), 1);
t.ensureIndex({geo: "2d", nonGeo: 1})
assert.eq(t.find({nonGeo: 123, geo: {$nearSphere: oldPoint}}).itcount(), 1);

t.drop()
t.insert({geo: oldPoint, nonGeo: [123,456]});
t.ensureIndex({geo: "2dsphere", nonGeo: 1})
assert.eq(t.find({nonGeo: 123, geo: {$nearSphere: oldPoint}}).itcount(), 1);

t.drop()
t.insert({geo: oldPoint, nonGeo: [123,456]});
t.ensureIndex({nonGeo: 1, geo: "2dsphere"})
assert.eq(t.find({nonGeo: 123, geo: {$nearSphere: oldPoint}}).itcount(), 1);
