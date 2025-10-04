// Test behavior of 2dsphere and sparse.  See SERVER-9639.
// All V2 2dsphere indices are sparse in the geo fields.
//
// @tags: [
//   # Some passthrough suites assume that consecutive CRUD operations can be grouped into
//   # a transaction that is short enough to succeed despite periodic stepdowns.
//   # Having several bulk inserts in a row violates this assumption: the retries take
//   # long enough that they will never succeed: they will always be interrupted by the stepdown.
//   operations_longer_than_stepdown_interval_in_txns,
// ]

const collNamePrefix = "geo_s2sparse_";
let collCount = 0;
let coll = db.getCollection(collNamePrefix + collCount++);

const point = {
    type: "Point",
    coordinates: [5, 5],
};
const indexSpec = {
    geo: "2dsphere",
    nonGeo: 1,
};
const indexName = "geo_2dsphere_nonGeo_1";

//
// V2 indices are "geo sparse" always.
//

// Clean up.
coll.drop();
coll.createIndex(indexSpec);

const bulkInsertDocs = function (coll, numDocs, makeDocFn) {
    print("Bulk inserting " + numDocs + " documents");

    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        bulk.insert(makeDocFn(i));
    }

    assert.commandWorked(bulk.execute());

    print("Bulk inserting " + numDocs + " documents completed");
};

// Insert N documents with the geo field.
const N = 1000;
bulkInsertDocs(coll, N, function (i) {
    return {geo: point, nonGeo: "point_" + i};
});

// Expect N keys.
assert.eq(N, coll.validate().keysPerIndex[indexName]);

// Insert N documents without the geo field.
bulkInsertDocs(coll, N, function (i) {
    return {wrongGeo: point, nonGeo: i};
});

// Still expect N keys as we didn't insert any geo stuff.
assert.eq(N, coll.validate().keysPerIndex[indexName]);

// Insert N documents with just the geo field.
bulkInsertDocs(coll, N, function (i) {
    return {geo: point};
});

// Expect 2N keys.
assert.eq(N + N, coll.validate().keysPerIndex[indexName]);

// Add some "not geo" stuff.
bulkInsertDocs(coll, N, function (i) {
    return {geo: null};
});
bulkInsertDocs(coll, N, function (i) {
    return {geo: []};
});
bulkInsertDocs(coll, N, function (i) {
    return {geo: undefined};
});
bulkInsertDocs(coll, N, function (i) {
    return {geo: {}};
});

// Still expect 2N keys.
assert.eq(N + N, coll.validate().keysPerIndex[indexName]);

//
// V1 indices are never sparse
//

coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
coll.createIndex(indexSpec, {"2dsphereIndexVersion": 1});

// Insert N documents with the geo field.
bulkInsertDocs(coll, N, function (i) {
    return {geo: point, nonGeo: "point_" + i};
});

// Expect N keys.
assert.eq(N, coll.validate().keysPerIndex[indexName]);

// Insert N documents without the geo field.
bulkInsertDocs(coll, N, function (i) {
    return {wrongGeo: point, nonGeo: i};
});

// Expect N keys as it's a V1 index.
assert.eq(N + N, coll.validate().keysPerIndex[indexName]);

//
// V2 indices with several 2dsphere-indexed fields are only sparse if all are missing.
//

// Clean up.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
coll.createIndex({geo: "2dsphere", otherGeo: "2dsphere"});

const indexNameOther = "geo_2dsphere_otherGeo_2dsphere";

// Insert N documents with the first geo field.
bulkInsertDocs(coll, N, function (i) {
    return {geo: point};
});

// Expect N keys.
assert.eq(N, coll.validate().keysPerIndex[indexNameOther]);

// Insert N documents with the second geo field.
bulkInsertDocs(coll, N, function (i) {
    return {otherGeo: point};
});

// They get inserted too.
assert.eq(N + N, coll.validate().keysPerIndex[indexNameOther]);

// Insert N documents with neither geo field.
bulkInsertDocs(coll, N, function (i) {
    return {nonGeo: i};
});

// Still expect 2N keys as the neither geo docs were omitted from the index.
assert.eq(N + N, coll.validate().keysPerIndex[indexNameOther]);
