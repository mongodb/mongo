// Test the sparseness of 2dsphere indices.
t = db.geo_s2sparse;
t.drop();

//
// Sparse on
//

// Insert a doc. that is indexed
pointA = { "type" : "Point", "coordinates": [ 40, 5 ] }
t.insert( {geo : pointA , nonGeo: "pointA"})
// Make a sparse index.
t.ensureIndex({geo: "2dsphere", otherNonGeo: 1}, {sparse: 1});

// Save the size of the index with one obj.
var sizeWithOneIndexedObj = t.stats().indexSizes.geo_2dsphere;

// Insert a bunch of data that won't be indexed.
var N = 1000;
for (var i = 0; i < N; ++i) {
    // Missing both.
    t.insert( {nonGeo: "pointA"})
    // Missing geo.
    t.insert( {geo: pointA})
    // Missing nonGeo.
    t.insert( {otherNonGeo: "pointA"})
}

// The size should be the same as we don't index no-geo data.
var sizeAfter = t.stats().indexSizes.geo_2dsphere;
assert.eq(sizeAfter, sizeWithOneIndexedObj);

//
// Sparse off
//

t.drop();

// Insert a doc. that is indexed
pointA = { "type" : "Point", "coordinates": [ 40, 5 ] }
t.insert( {geo : pointA , nonGeo: "pointA"})
// Make a non-sparse index.
t.ensureIndex({geo: "2dsphere"});

// Save the size of the index with one obj.
var sizeWithOneIndexedObj = t.stats().indexSizes.geo_2dsphere;

// Insert a bunch of data that will be indexed with the "no geo field" key
var N = 1000;
for (var i = 0; i < N; ++i) {
    t.insert( {nonGeo: "pointA"})
}

// Since the index isn't sparse we have entries in the index for each insertion even though it's
// missing the geo key.
var sizeAfter = t.stats().indexSizes.geo_2dsphere;
assert.gt(sizeAfter, sizeWithOneIndexedObj);
