// Explode arrays when indexing non-geo fields in 2dsphere, and make sure that
// we find them with queries.
// @tags: [
//   requires_getmore,
// ]

let t = db.geo_s2nongeoarray;

let oldPoint = [40, 5];

let data = {geo: oldPoint, nonGeo: [123, 456], otherNonGeo: [{b: [1, 2]}, {b: [3, 4]}]};

t.drop();
assert.commandWorked(t.insert(data));
assert.commandWorked(t.createIndex({otherNonGeo: 1}));
assert.eq(1, t.find({otherNonGeo: {b: [1, 2]}}).itcount());
assert.eq(0, t.find({otherNonGeo: 1}).itcount());
assert.eq(1, t.find({"otherNonGeo.b": 1}).itcount());

t.drop();
t.insert(data);
t.createIndex({geo: "2d", nonGeo: 1, otherNonGeo: 1});
assert.eq(t.find({nonGeo: 123, geo: {$nearSphere: oldPoint}}).itcount(), 1);
assert.eq(t.find({"otherNonGeo.b": 1, geo: {$nearSphere: oldPoint}}).itcount(), 1);

t.drop();
t.insert(data);
t.createIndex({geo: "2dsphere", nonGeo: 1, otherNonGeo: 1});
assert.eq(t.find({nonGeo: 123, geo: {$nearSphere: oldPoint}}).itcount(), 1);
assert.eq(t.find({"otherNonGeo.b": 1, geo: {$nearSphere: oldPoint}}).itcount(), 1);
