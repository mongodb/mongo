// Test the sparseness of 2dsphere indices.
t = db.geo_s2sparse;
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

// The size should be the same as we don't index the missing data.
var sizeAfter = t.stats().indexSizes.geo_2dsphere;
assert.eq(sizeAfter, sizeWithOneIndexedObj);
