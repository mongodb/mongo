// Test behavior of 2dsphere and sparse.  See SERVER-9639.
// All V2 2dsphere indices are sparse in the geo fields.

var coll = db.geo_s2sparse;

var point = {type: "Point", coordinates: [5, 5]};

var indexSpec = {geo: "2dsphere", nonGeo: 1};

var indexName = 'test.geo_s2sparse.$geo_2dsphere_nonGeo_1';

//
// V2 indices are "geo sparse" always.
//

// Clean up.
coll.drop();
coll.ensureIndex(indexSpec);

// Insert N documents with the geo field.
var N = 1000;
for (var i = 0; i < N; i++) {
    coll.insert({geo: point, nonGeo: "point_" + i});
}

// Expect N keys.
assert.eq(N, coll.validate().keysPerIndex[indexName]);

// Insert N documents without the geo field.
for (var i = 0; i < N; i++) {
    coll.insert({wrongGeo: point, nonGeo: i});
}

// Still expect N keys as we didn't insert any geo stuff.
assert.eq(N, coll.validate().keysPerIndex[indexName]);

// Insert N documents with just the geo field.
for (var i = 0; i < N; i++) {
    coll.insert({geo: point});
}

// Expect 2N keys.
assert.eq(N + N, coll.validate().keysPerIndex[indexName]);

// Add some "not geo" stuff.
for (var i = 0; i < N; i++) {
    coll.insert({geo: null});
    coll.insert({geo: []});
    coll.insert({geo: undefined});
    coll.insert({geo: {}});
}

// Still expect 2N keys.
assert.eq(N + N, coll.validate().keysPerIndex[indexName]);

//
// V1 indices are never sparse
//

coll.drop();
coll.ensureIndex(indexSpec, {"2dsphereIndexVersion": 1});

// Insert N documents with the geo field.
for (var i = 0; i < N; i++) {
    coll.insert({geo: point, nonGeo: "point_" + i});
}

// Expect N keys.
assert.eq(N, coll.validate().keysPerIndex[indexName]);

// Insert N documents without the geo field.
for (var i = 0; i < N; i++) {
    coll.insert({wrongGeo: point, nonGeo: i});
}

// Expect N keys as it's a V1 index.
assert.eq(N + N, coll.validate().keysPerIndex[indexName]);

//
// V2 indices with several 2dsphere-indexed fields are only sparse if all are missing.
//

// Clean up.
coll.drop();
coll.ensureIndex({geo: "2dsphere", otherGeo: "2dsphere"});

indexName = 'test.geo_s2sparse.$geo_2dsphere_otherGeo_2dsphere';

// Insert N documents with the first geo field.
var N = 1000;
for (var i = 0; i < N; i++) {
    coll.insert({geo: point});
}

// Expect N keys.
assert.eq(N, coll.validate().keysPerIndex[indexName]);

// Insert N documents with the second geo field.
var N = 1000;
for (var i = 0; i < N; i++) {
    coll.insert({otherGeo: point});
}

// They get inserted too.
assert.eq(N + N, coll.validate().keysPerIndex[indexName]);

// Insert N documents with neither geo field.
for (var i = 0; i < N; i++) {
    coll.insert({nonGeo: i});
}

// Still expect 2N keys as the neither geo docs were omitted from the index.
assert.eq(N + N, coll.validate().keysPerIndex[indexName]);
