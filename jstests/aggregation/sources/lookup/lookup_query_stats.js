/**
 * Tests that the queryExecutor stats are correctly returned when $lookup is performed on
 * foreign collection.
 *
 * @tags: [
 *     assumes_unsharded_collection,
 *     assumes_no_implicit_collection_creation_after_drop,
 *     do_not_wrap_aggregations_in_facets,
 *     assumes_read_preference_unchanged,
 *     assumes_read_concern_unchanged,
 *     assumes_against_mongod_not_mongos
 * ]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB("lookup_query_stats");
testDB.dropDatabase();

const localColl = testDB.getCollection("local");
const fromColl = testDB.getCollection("foreign");
const foreignDocCount = 10;
const localDocCount = 2;

// Keeps track of the last query execution stats.
let lastScannedObjects = 0;
let lastScannedKeys = 0;

let insertDocumentToCollection = function(collection, docCount, fieldName) {
    const bulk = collection.initializeUnorderedBulkOp();
    for (let i = 0; i < docCount; i++) {
        let doc = {_id: i};
        doc[fieldName] = i;
        bulk.insert(doc);
    }
    assert.commandWorked(bulk.execute());
};

let doAggregationLookup = function(localColl, fromColl) {
    return localColl.aggregate([
        {
            $lookup: {
                from: fromColl.getName(),
                localField: "localField",
                foreignField: "foreignField",
                as: "output"
            }
        },
        {
            $sort: {_id: 1}
        }]).toArray();
};

let getCurentQueryExecutorStats = function() {
    let queryExecutor = testDB.serverStatus().metrics.queryExecutor;

    let curScannedObjects = queryExecutor.scannedObjects - lastScannedObjects;
    let curScannedKeys = queryExecutor.scanned - lastScannedKeys;

    lastScannedObjects = queryExecutor.scannedObjects;
    lastScannedKeys = queryExecutor.scanned;

    return [curScannedObjects, curScannedKeys];
};

let testQueryExecutorStatsWithCollectionScan = function() {
    let output = doAggregationLookup(localColl, fromColl);

    let expectedOutput = [
        {_id: 0, localField: 0, output: [{_id: 0, foreignField: 0}]},
        {_id: 1, localField: 1, output: [{_id: 1, foreignField: 1}]}
    ];

    assert.eq(output, expectedOutput);

    let [curScannedObjects, curScannedKeys] = getCurentQueryExecutorStats();

    // For collection scan, total scannedObjects should be sum of
    // (total documents in local collection +
    //  total documents in local collection * total documents in foreign collection)
    assert.eq(localDocCount + localDocCount * foreignDocCount, curScannedObjects);

    // There is no index in the collection.
    assert.eq(0, curScannedKeys);
};

let createIndexForCollection = function(collection, fieldName) {
    let request = {};
    request[fieldName] = 1;
    assert.commandWorked(collection.createIndex(request));
};

let testQueryExecutorStatsWithIndexScan = function() {
    createIndexForCollection(fromColl, "foreignField");

    let output = doAggregationLookup(localColl, fromColl);

    let expectedOutput = [
        {_id: 0, localField: 0, output: [{_id: 0, foreignField: 0}]},
        {_id: 1, localField: 1, output: [{_id: 1, foreignField: 1}]}
    ];

    assert.eq(output, expectedOutput);

    let [curScannedObjects, curScannedKeys] = getCurentQueryExecutorStats();

    // For index scan, total scannedObjects should be sum of
    // (total documents in local collection + total matched documents in foreign collection)
    assert.eq(localDocCount + localDocCount, curScannedObjects);

    // Number of keys scanned in the foreign collection should be equal number of keys in local
    // collection.
    assert.eq(localDocCount, curScannedKeys);
};

insertDocumentToCollection(fromColl, foreignDocCount, "foreignField");
insertDocumentToCollection(localColl, localDocCount, "localField");

// This test might be called over an existing MongoD instance. We should populate
// lastScannedObjects and lastScannedKeys with existing stats values in that case.
getCurentQueryExecutorStats();

testQueryExecutorStatsWithCollectionScan();
testQueryExecutorStatsWithIndexScan();
}());
