// Test that 2dsphere ignores the sparse option.

var coll = db.geo_s2sparse;
coll.drop();

// 2d geo object used in inserted documents
var point = { type: "Point", coordinates: [5, 5] }

// the index we'll use
var index = { geo: "2dsphere", nonGeo: 1 };
var indexName = "geo_2dsphere_nonGeo_1";

// ensure a sparse index
coll.ensureIndex(index, { sparse: 1 });

// note the initial size of the index
var initialIndexSize = coll.stats().indexSizes[indexName];
assert.gte(initialIndexSize, 0);

// insert matching docs
for (var i = 0; i < 1000; i++) {
    coll.insert({ geo: point, nonGeo: "point_"+i });
}

// the new size of the index
var indexSizeAfterFirstInsert = coll.stats().indexSizes[indexName];
assert.gte(indexSizeAfterFirstInsert, initialIndexSize);

// insert docs missing the nonGeo field
for (var i = 0; i < 1000; i++) {
    coll.insert({ geo: point, wrongNonGeo: "point_"+i });
}

// the new size, should be bigger
var indexSizeAfterSecondInsert = coll.stats().indexSizes[indexName];
assert.gte(indexSizeAfterSecondInsert, indexSizeAfterFirstInsert);

// insert docs missing the geo field, to make sure they're filtered out
for (var i = 0; i < 1000; i++) {
    coll.insert({ wrongGeo: point, nonGeo: "point_"+i });
}

// the new size, should be unchanged
var indexSizeAfterThirdInsert = coll.stats().indexSizes[indexName];
assert.gte(indexSizeAfterThirdInsert, indexSizeAfterSecondInsert);
