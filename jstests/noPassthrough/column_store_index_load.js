
/**
 * Test that different methods of loading a column store index all produce the same valid results.
 * Indexes are validated by comparing query results that use the index with results from a control
 * query that uses a collection scan.
 * @tags: [
 *   # columnstore indexes are new in 6.1.
 *   requires_fcv_61,
 *   # We could potentially need to resume an index build in the event of a stepdown, which is not
 *   # yet implemented.
 *   does_not_support_stepdowns,
 *   # Columnstore indexes are incompatible with clustered collections.
 *   incompatible_with_clustered_collection,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const mongod = MongoRunner.runMongod({});
const db = mongod.getDB("test");

const columnStoreEnabled =
    checkSBEEnabled(db, ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"]);
if (!columnStoreEnabled) {
    jsTestLog("Skipping column store bulk load test test since the feature flag is not enabled.");
    MongoRunner.stopMongod(mongod);
    return;
}

//
// Create test documents.
//

const testValues = [
    {foo: 1, bar: 2},
    {bar: 2, baz: 3},
    {foo: 3, baz: 4},
    {foo: 5, bar: 6},
    {bar: 7, baz: [7, 8]},
];

// We create our test documents by choosing k-permutations from the 'testValues' array. The
// kPermutations() function returns an array of all possible permutations with 'k' choices from all
// values in the 'arr' input. The 'choices' input stores the values chosen at previous levels of
// recursion.
function kPermutations(arr, n, choices = []) {
    if (n == 0) {
        return [choices];
    }

    const permutations = [];
    for (let i = 0; i < arr.length; ++i) {
        const subSequence = arr.slice(0, i).concat(arr.slice(i + 1));
        permutations.push(...kPermutations(subSequence, n - 1, choices.concat([arr[i]])));
    }
    return permutations;
}

const testDocs = kPermutations(testValues, 4).map((permutation, idx) => ({
                                                      idx: idx,
                                                      foo: [permutation[0], permutation[1]],
                                                      bar: [permutation[2], permutation[3]]
                                                  }));

// Test queries use a projection that includes every possible leaf field. Projections on fields that
// have sub-documents fall back to the row store, which would not serve to validate the contents of
// the index.
const testProjection = {
    _id: 0,
    idx: 1,
    "foo.foo": 1,
    "foo.bar": 1,
    "foo.baz": 1,
    "bar.foo": 1,
    "bar.bar": 1,
    "bar.baz": 1,
};

const maxMemUsageBytes = 20000;
const numDocs = testDocs.length;
const approxDocSize = 800;
const approxMemoryUsage = numDocs * approxDocSize;
const expectedSpilledRanges = Math.ceil(approxMemoryUsage / maxMemUsageBytes);

// The test query would normally not qualify for a column store index plan, because it projects a
// large number of fields. We raise the limit on the number of fields to allow column store plans
// for the purposes of this test.
db.adminCommand({
    setParameter: 1,
    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan: Object.keys(testProjection).length
});

function loadDocs(coll, documents) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (const doc of documents) {
        bulk.insert(doc);
    }
    assert.commandWorked(bulk.execute());
}

//
// We load the same documents into 4 collections:
//
//   1. a control collection with no index,
const noIndexColl = db.column_store_index_load_no_index;

//   2. a collection whose column store index is populated with an in-memory bulk load,
const bulkLoadInMemoryColl = db.column_store_index_load_in_memory;

//   3. a collection whose column store index is populated with a bulk load that uses an external
//   merge sort (i.e., one that "spills" to disk), and
const bulkLoadExternalColl = db.column_store_index_load_external;

//   4. a collection whose column store index is populated as documents are inserted.
const onlineLoadColl = db.column_store_index_online_load;

// Load the control collection.
noIndexColl.drop();
loadDocs(noIndexColl, testDocs);

// Perform the in-memory bulk load.
bulkLoadInMemoryColl.drop();
loadDocs(bulkLoadInMemoryColl, testDocs);
assert.commandWorked(bulkLoadInMemoryColl.createIndex({"$**": "columnstore"}));

const statsAfterInMemoryBuild = assert.commandWorked(db.runCommand({serverStatus: 1}));
assert.docEq({
    count: NumberLong(1),
    resumed: NumberLong(0),
    filesOpenedForExternalSort: NumberLong(0),
    filesClosedForExternalSort: NumberLong(0),
    spilledRanges: NumberLong(0),
    bytesSpilled: NumberLong(0),
},
             statsAfterInMemoryBuild.indexBulkBuilder);

// Perform the external bulk load. The server config won't allow a memory limit lower than 50MB, so
// we use a failpoint to set it lower than that for the purposes of this test.
bulkLoadExternalColl.drop();
assert.commandWorked(db.adminCommand({
    configureFailPoint: "constrainMemoryForBulkBuild",
    mode: "alwaysOn",
    data: {maxBytes: maxMemUsageBytes},
}));
loadDocs(bulkLoadExternalColl, testDocs);
assert.commandWorked(bulkLoadExternalColl.createIndex({"$**": "columnstore"}));

const statsAfterExternalLoad = assert.commandWorked(db.runCommand({serverStatus: 1}));
let indexBulkBuilderSection = statsAfterExternalLoad.indexBulkBuilder;
assert.eq(indexBulkBuilderSection.count, 2, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.resumed, 0, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesOpenedForExternalSort, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesClosedForExternalSort, 1, tojson(indexBulkBuilderSection));
// Note: The number of spills in the external sorter depends on the size of C++ data structures,
// which can be different between architectures. The test allows a range of reasonable values.
assert.between(expectedSpilledRanges - 1,
               indexBulkBuilderSection.spilledRanges,
               expectedSpilledRanges + 1,
               tojson(indexBulkBuilderSection),
               true);
// We can only approximate the memory usage and bytes that will be spilled.
assert.between(0,
               indexBulkBuilderSection.bytesSpilled,
               approxMemoryUsage,
               tojson(indexBulkBuilderSection),
               true);

// Perfom the online load.
onlineLoadColl.drop();
onlineLoadColl.createIndex({"$**": "columnstore"});
loadDocs(onlineLoadColl, testDocs);

//
// Verify that our test query uses the column store.
//
[bulkLoadInMemoryColl, bulkLoadExternalColl, onlineLoadColl].forEach(function(coll) {
    const explain = coll.find({}, testProjection).sort({idx: 1}).explain();
    assert(planHasStage(db, explain, "COLUMN_SCAN"), explain);
});

//
// Run a query on each of the test collections, including the "no index" control collection.
//
const noIndexResults = noIndexColl.find({}, testProjection).sort({idx: 1}).toArray();
const bulkLoadInMemoryResults =
    bulkLoadInMemoryColl.find({}, testProjection).sort({idx: 1}).toArray();
const bulkLoadExternalResults =
    bulkLoadExternalColl.find({}, testProjection).sort({idx: 1}).toArray();
const onlineLoadResults = onlineLoadColl.find({}, testProjection).sort({idx: 1}).toArray();

//
// Verify that the test query produces the same results in all test configurations.
//
assert.eq(testDocs.length, noIndexResults.length);
assert.eq(testDocs.length, bulkLoadInMemoryResults.length);
assert.eq(testDocs.length, bulkLoadExternalResults.length);
assert.eq(testDocs.length, onlineLoadResults.length);

for (let i = 0; i < noIndexResults.length; ++i) {
    assert.docEq(noIndexResults[i], bulkLoadInMemoryResults[i]);
    assert.docEq(noIndexResults[i], bulkLoadExternalResults[i]);
    assert.docEq(noIndexResults[i], onlineLoadResults[i]);
}

MongoRunner.stopMongod(mongod);
})();
