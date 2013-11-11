// the collection we will be using
var coll = db.geo_s2sparse;

// 2d geo object used in inserted documents
var point = { type: "Point", coordinates: [5, 5] }

// the index we'll use
var index = { geo: "2dsphere", nonGeo: 1 };
var indexName = "geo_2dsphere_nonGeo_1";

/// First run: make sure a sparse 2dsphere index behaves correctly

// clean up
coll.drop();

// ensure a sparse index
coll.ensureIndex(index, { sparse: 1 });

// note the initial size of the index
var initialIndexSize = coll.stats().indexSizes[indexName];
assert.gt(initialIndexSize, 0);

// insert matching docs
for (var i = 0; i < 1000; i++) {
    coll.insert({ geo: point, nonGeo: "point_"+i });
}

// the new size of the index
var indexSizeAfterFirstInsert = coll.stats().indexSizes[indexName];
assert.gt(indexSizeAfterFirstInsert, initialIndexSize);

// insert docs missing the nonGeo field
for (var i = 0; i < 1000; i++) {
    coll.insert({ geo: point, wrongNonGeo: "point_"+i });
}

// the new size, should be unchanged
var indexSizeAfterSecondInsert = coll.stats().indexSizes[indexName];
assert.eq(indexSizeAfterFirstInsert, indexSizeAfterSecondInsert);

// insert docs missing the geo field, to make sure they're filtered out
for (var i = 0; i < 1000; i++) {
    coll.insert({ wrongGeo: point, nonGeo: "point_"+i });
}

// the new size, should be unchanged
var indexSizeAfterThirdInsert = coll.stats().indexSizes[indexName];
assert.eq(indexSizeAfterSecondInsert, indexSizeAfterThirdInsert);

/// Second run: make sure a non-sparse 2dsphere index behaves correctly

// clean up
coll.drop();

// ensure a normal (non-sparse) index
coll.ensureIndex(index);

// note the initial size of the index
initialIndexSize = coll.stats().indexSizes[indexName];
assert.gt(initialIndexSize, 0);

// insert matching docs
for (var i = 0; i < 1000; i++) {
    coll.insert({ geo: point, nonGeo: "point_"+i });
}

// the new size of the index
indexSizeAfterFirstInsert = coll.stats().indexSizes[indexName];
assert.gt(indexSizeAfterFirstInsert, initialIndexSize);

// insert docs missing the nonGeo field, which should still be indexed
for (var i = 0; i < 1000; i++) {
    coll.insert({ geo: point, wrongNonGeo: "point_"+i });
}

// the new size, which should be larger with the new index entries
indexSizeAfterSecondInsert = coll.stats().indexSizes[indexName];
assert.gt(indexSizeAfterSecondInsert, indexSizeAfterFirstInsert);

// insert docs missing the geo field, which should still be indexed
for (var i = 0; i < 1000; i++) {
    coll.insert({ wrongGeo: point, nonGeo: "point_"+i });
}

// the new size
indexSizeAfterThirdInsert = coll.stats().indexSizes[indexName];
assert.gt(indexSizeAfterThirdInsert, indexSizeAfterSecondInsert);

// insert docs missing both fields, which should still be indexed
for (var i = 0; i < 1000; i++) {
    coll.insert({ wrongGeo: point, wrongNonGeo: "point_"+i });
}

// the new size
var indexSizeAfterFourthInsert = coll.stats().indexSizes[indexName];
assert.gt(indexSizeAfterFourthInsert, indexSizeAfterThirdInsert);
