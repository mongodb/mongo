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

load("jstests/libs/analyze_plan.js");  // for 'getAggPlanStages'

const testDB = db.getSiblingDB("lookup_query_stats");
testDB.dropDatabase();

const localColl = testDB.getCollection("local");
const fromColl = testDB.getCollection("foreign");
const foreignDocCount = 10;
const localDocCount = 2;

const kExecutionStats = "executionStats";
const kAllPlansExecution = "allPlansExecution";
const kQueryPlanner = "queryPlanner";

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

let aggregationLookupPipeline = function(localColl, fromColl) {
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
            $sort: {localField: 1}
        }]);
};

let doAggregationLookup = function(localColl, fromColl) {
    return aggregationLookupPipeline(localColl, fromColl).toArray();
};

let explainAggregationLookup = function(localColl, fromColl, verbosityLevel) {
    return aggregationLookupPipeline(localColl.explain(verbosityLevel), fromColl);
};

let getCurrentQueryExecutorStats = function() {
    let queryExecutor = testDB.serverStatus().metrics.queryExecutor;

    let curScannedObjects = queryExecutor.scannedObjects - lastScannedObjects;
    let curScannedKeys = queryExecutor.scanned - lastScannedKeys;

    lastScannedObjects = queryExecutor.scannedObjects;
    lastScannedKeys = queryExecutor.scanned;

    return [curScannedObjects, curScannedKeys];
};

let checkExplainOutputForVerLevel = function(explainOutput, expected, verbosityLevel) {
    let lkpStages = getAggPlanStages(explainOutput, "$lookup");
    assert.eq(lkpStages.length, 1, lkpStages);
    let lkpStage = lkpStages[0];
    if (verbosityLevel && verbosityLevel !== kQueryPlanner) {
        assert(lkpStage.hasOwnProperty("totalDocsExamined"), lkpStage);
        assert.eq(lkpStage.totalDocsExamined, expected.totalDocsExamined, lkpStage);
        assert(lkpStage.hasOwnProperty("totalKeysExamined"), lkpStage);
        assert.eq(lkpStage.totalKeysExamined, expected.totalKeysExamined, lkpStage);
        assert(lkpStage.hasOwnProperty("collectionScans"), lkpStage);
        assert.eq(lkpStage.collectionScans, expected.collectionScans, lkpStage);
        assert(lkpStage.hasOwnProperty("indexesUsed"), lkpStage);
        assert(Array.isArray(lkpStage.indexesUsed), lkpStage);
        assert.eq(lkpStage.indexesUsed, expected.indexesUsed, lkpStage);
    } else {  // If no `verbosityLevel` is passed or 'queryPlanner' is passed.
        assert(!lkpStage.hasOwnProperty("totalDocsExamined"), lkpStage);
        assert(!lkpStage.hasOwnProperty("totalKeysExamined"), lkpStage);
        assert(!lkpStage.hasOwnProperty("collectionScans"), lkpStage);
        assert(!lkpStage.hasOwnProperty("indexesUsed"), lkpStage);
    }
};

let checkExplainOutputForAllVerbosityLevels = function(localColl, fromColl, expectedExplainResult) {
    // The `explain` verbosity level: 'allPlansExecution'.
    let explainAllPlansOutput = explainAggregationLookup(localColl, fromColl, kAllPlansExecution);
    checkExplainOutputForVerLevel(explainAllPlansOutput, expectedExplainResult, kAllPlansExecution);

    // The `explain` verbosity level: 'executionStats'.
    let explainExecStatsOutput = explainAggregationLookup(localColl, fromColl, kExecutionStats);
    checkExplainOutputForVerLevel(explainExecStatsOutput, expectedExplainResult, kExecutionStats);

    // The `explain` verbosity level: 'queryPlanner'.
    let explainQueryPlannerOutput = explainAggregationLookup(localColl, fromColl, kQueryPlanner);
    checkExplainOutputForVerLevel(explainQueryPlannerOutput, expectedExplainResult, kQueryPlanner);

    // The `explain` verbosity level is not passed.
    let explainOutput = explainAggregationLookup(localColl, fromColl);
    checkExplainOutputForVerLevel(explainOutput, expectedExplainResult);
};

let testQueryExecutorStatsWithCollectionScan = function() {
    let output = doAggregationLookup(localColl, fromColl);

    let expectedOutput = [
        {_id: 0, localField: 0, output: [{_id: 0, foreignField: 0}]},
        {_id: 1, localField: 1, output: [{_id: 1, foreignField: 1}]}
    ];

    assert.eq(output, expectedOutput);

    let [curScannedObjects, curScannedKeys] = getCurrentQueryExecutorStats();

    // For collection scan, total scannedObjects should be sum of
    // (total documents in local collection +
    //  total documents in local collection * total documents in foreign collection)
    assert.eq(localDocCount + localDocCount * foreignDocCount, curScannedObjects);

    // There is no index in the collection.
    assert.eq(0, curScannedKeys);

    let expectedExplainResult =
        {totalDocsExamined: 20, totalKeysExamined: 0, collectionScans: 4, indexesUsed: []};
    checkExplainOutputForAllVerbosityLevels(localColl, fromColl, expectedExplainResult);
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

    let [curScannedObjects, curScannedKeys] = getCurrentQueryExecutorStats();

    // For index scan, total scannedObjects should be sum of
    // (total documents in local collection + total matched documents in foreign collection)
    assert.eq(localDocCount + localDocCount, curScannedObjects);

    // Number of keys scanned in the foreign collection should be equal number of keys in local
    // collection.
    assert.eq(localDocCount, curScannedKeys);

    let expectedExplainResult = {
        totalDocsExamined: 2,
        totalKeysExamined: 2,
        collectionScans: 0,
        indexesUsed: ["foreignField_1"]
    };
    checkExplainOutputForAllVerbosityLevels(localColl, fromColl, expectedExplainResult);
};

insertDocumentToCollection(fromColl, foreignDocCount, "foreignField");
insertDocumentToCollection(localColl, localDocCount, "localField");

// This test might be called over an existing MongoD instance. We should populate
// lastScannedObjects and lastScannedKeys with existing stats values in that case.
getCurrentQueryExecutorStats();

testQueryExecutorStatsWithCollectionScan();
testQueryExecutorStatsWithIndexScan();
}());
