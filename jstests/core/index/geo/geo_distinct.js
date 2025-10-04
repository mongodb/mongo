// Tests distinct with geospatial field values.
// 1. Test distinct with geo values for 'key' (SERVER-2135)
// 2. Test distinct with geo predicates for 'query' (SERVER-13769)
//
// @tags: [
//   requires_fastcount,
// ]

const coll = db.geo_distinct;
let res;

//
// 1. Test distinct with geo values for 'key'.
//

coll.drop();
assert.commandWorked(coll.insert({loc: {type: "Point", coordinates: [10, 20]}}));
assert.commandWorked(coll.insert({loc: {type: "Point", coordinates: [10, 20]}}));
assert.commandWorked(coll.insert({loc: {type: "Point", coordinates: [20, 30]}}));
assert.commandWorked(coll.insert({loc: {type: "Point", coordinates: [20, 30]}}));
assert.eq(4, coll.count());

// Test distinct on GeoJSON points with/without a 2dsphere index.

res = coll.runCommand("distinct", {key: "loc"});
assert.commandWorked(res);
assert.eq(res.values.sort(bsonWoCompare), [
    {type: "Point", coordinates: [10, 20]},
    {type: "Point", coordinates: [20, 30]},
]);

assert.commandWorked(coll.createIndex({loc: "2dsphere"}));

res = coll.runCommand("distinct", {key: "loc"});
assert.commandWorked(res);
assert.eq(res.values.sort(bsonWoCompare), [
    {type: "Point", coordinates: [10, 20]},
    {type: "Point", coordinates: [20, 30]},
]);

// Test distinct on legacy points with/without a 2d index.

// (Note that distinct on a 2d-indexed field doesn't produce a list of coordinate pairs, since
// distinct logically operates on unique values in an array. Hence, the results are unintuitive
// and not semantically meaningful.)

assert.commandWorked(coll.dropIndexes());

res = coll.runCommand("distinct", {key: "loc.coordinates"});
assert.commandWorked(res);
assert.eq(res.values.sort(), [10, 20, 30]);

assert.commandWorked(coll.createIndex({"loc.coordinates": "2d"}));

res = coll.runCommand("distinct", {key: "loc.coordinates"});
assert.commandWorked(res);
assert.eq(res.values.sort(), [10, 20, 30]);

//
// 2. Test distinct with geo predicates for 'query'.
//

assert(coll.drop());
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 50; ++i) {
    bulk.insert({zone: 1, loc: {type: "Point", coordinates: [-20, -20]}});
    bulk.insert({zone: 2, loc: {type: "Point", coordinates: [-10, -10]}});
    bulk.insert({zone: 3, loc: {type: "Point", coordinates: [0, 0]}});
    bulk.insert({zone: 4, loc: {type: "Point", coordinates: [10, 10]}});
    bulk.insert({zone: 5, loc: {type: "Point", coordinates: [20, 20]}});
}
assert.commandWorked(bulk.execute());

const originGeoJSON = {
    type: "Point",
    coordinates: [0, 0],
};

// Test distinct with $nearSphere query predicate.

// A. Unindexed key, no geo index on query predicate.
res = coll.runCommand("distinct", {
    key: "zone",
    query: {loc: {$nearSphere: {$geometry: originGeoJSON, $maxDistance: 1}}},
});
assert.commandFailed(res);
// B. Unindexed key, with 2dsphere index on query predicate.
assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
res = coll.runCommand("distinct", {
    key: "zone",
    query: {loc: {$nearSphere: {$geometry: originGeoJSON, $maxDistance: 1}}},
});
assert.commandWorked(res);
assert.eq(res.values.sort(), [3]);
// C. Indexed key, with 2dsphere index on query predicate.
assert.commandWorked(coll.createIndex({zone: 1}));
res = coll.runCommand("distinct", {
    key: "zone",
    query: {loc: {$nearSphere: {$geometry: originGeoJSON, $maxDistance: 1}}},
});
assert.commandWorked(res);
assert.eq(res.values.sort(), [3]);

// Test distinct with $near query predicate.

assert.commandWorked(coll.dropIndexes());

// A. Unindexed key, no geo index on query predicate.
res = coll.runCommand("distinct", {key: "zone", query: {"loc.coordinates": {$near: [0, 0], $maxDistance: 1}}});
assert.commandFailed(res);
// B. Unindexed key, with 2d index on query predicate.
assert.commandWorked(coll.createIndex({"loc.coordinates": "2d"}));
res = coll.runCommand("distinct", {key: "zone", query: {"loc.coordinates": {$near: [0, 0], $maxDistance: 1}}});
assert.commandWorked(res);
assert.eq(res.values.sort(), [3]);
// C. Indexed key, with 2d index on query predicate.
assert.commandWorked(coll.createIndex({zone: 1}));
res = coll.runCommand("distinct", {key: "zone", query: {"loc.coordinates": {$near: [0, 0], $maxDistance: 1}}});
assert.commandWorked(res);
assert.eq(res.values.sort(), [3]);
