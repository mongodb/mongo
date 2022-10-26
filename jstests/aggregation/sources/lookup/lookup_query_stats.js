/**
 * Tests that the queryExecutor stats are correctly returned when $lookup is performed on
 * foreign collection.
 *
 * @tags: [
 *     # Should not run on sharded suites due to use of serverStatus()
 *     assumes_unsharded_collection,
 *     assumes_no_implicit_collection_creation_after_drop,
 *     do_not_wrap_aggregations_in_facets,
 *     assumes_read_preference_unchanged,
 *     assumes_read_concern_unchanged,
 *     assumes_against_mongod_not_mongos,
 *     does_not_support_repeated_reads,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");         // For 'getAggPlanStages'
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.
load("jstests/libs/sbe_explain_helpers.js");  // For getSbePlanStages and
                                              // getQueryInfoAtTopLevelOrFirstStage.

const isSBELookupEnabled = checkSBEEnabled(db);
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

let aggregationLookupPipeline = function(localColl, fromColl, allowDiskUse) {
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
        }], allowDiskUse);
};

let doAggregationLookup = function(localColl, fromColl, allowDiskUse) {
    return aggregationLookupPipeline(localColl, fromColl, allowDiskUse).toArray();
};

let explainAggregationLookup = function(localColl, fromColl, verbosityLevel, allowDiskUse) {
    return aggregationLookupPipeline(localColl.explain(verbosityLevel), fromColl, allowDiskUse);
};

let getCurrentQueryExecutorStats = function() {
    let queryExecutor = testDB.serverStatus().metrics.queryExecutor;

    let curScannedObjects = queryExecutor.scannedObjects - lastScannedObjects;
    let curScannedKeys = queryExecutor.scanned - lastScannedKeys;

    lastScannedObjects = queryExecutor.scannedObjects;
    lastScannedKeys = queryExecutor.scanned;

    return [curScannedObjects, curScannedKeys];
};

let checkExplainOutputForVerLevel = function(
    explainOutput, expected, verbosityLevel, expectedQueryPlan) {
    const lkpStages = getAggPlanStages(explainOutput, "$lookup");

    // Only make SBE specific assertions when we know that our $lookup has been pushed down.
    if (isSBELookupEnabled) {
        // If the SBE lookup is enabled, the $lookup stage is pushed down to the SBE and it's
        // not visible in 'stages' field of the explain output. Instead, 'queryPlan.stage' must be
        // "EQ_LOOKUP".
        assert.eq(lkpStages.length, 0, lkpStages, explainOutput);
        const queryInfo = getQueryInfoAtTopLevelOrFirstStage(explainOutput);
        const planner = queryInfo.queryPlanner;
        assert(planner.hasOwnProperty("winningPlan") &&
                   planner.winningPlan.hasOwnProperty("queryPlan"),
               explainOutput);
        const plan = planner.winningPlan.queryPlan;
        assert(plan.hasOwnProperty("stage") && plan.stage == "EQ_LOOKUP", explainOutput);
        assert(expectedQueryPlan.hasOwnProperty("strategy"), expectedQueryPlan);
        assert(plan.hasOwnProperty("strategy") && plan.strategy == expectedQueryPlan.strategy,
               explainOutput);
        if (expectedQueryPlan.strategy == "IndexedLoopJoin") {
            assert(plan.hasOwnProperty("indexName"), expectedQueryPlan);
            assert.eq(plan.indexName, expectedQueryPlan.indexName, explainOutput);
        }

        const expectedTopLevelJoinStage =
            expectedQueryPlan.strategy == "HashJoin" ? "hash_lookup" : "nlj";

        const sbeNljStages = getSbePlanStages(explainOutput, expectedTopLevelJoinStage);
        if (verbosityLevel && verbosityLevel !== kQueryPlanner) {
            assert.gt(sbeNljStages.length, 0, explainOutput);
            const topNljStage = sbeNljStages[0];

            assert(topNljStage.hasOwnProperty("totalDocsExamined"), explainOutput);
            assert.eq(topNljStage.totalDocsExamined, expected.totalDocsExamined, explainOutput);
            assert(topNljStage.hasOwnProperty("totalKeysExamined"), explainOutput);
            assert.eq(topNljStage.totalKeysExamined, expected.totalKeysExamined, explainOutput);

            assert(topNljStage.hasOwnProperty("collectionScans"), explainOutput);
            assert.eq(topNljStage.collectionScans, expected.collectionScans, explainOutput);
            assert(topNljStage.hasOwnProperty("collectionSeeks"), explainOutput);
            assert.eq(topNljStage.collectionSeeks, expected.collectionSeeks, explainOutput);
            assert(topNljStage.hasOwnProperty("indexScans"), explainOutput);
            assert.eq(topNljStage.indexScans, expected.indexScans, explainOutput);
            assert(topNljStage.hasOwnProperty("indexSeeks"), explainOutput);
            assert.eq(topNljStage.indexSeeks, expected.indexSeeks, explainOutput);
            assert(topNljStage.hasOwnProperty("indexesUsed"), explainOutput);
            assert(Array.isArray(topNljStage.indexesUsed), explainOutput);
            assert.eq(topNljStage.indexesUsed, expected.indexesUsed, explainOutput);
        } else {  // If no `verbosityLevel` is passed or 'queryPlanner' is passed.
            assert(!plan.hasOwnProperty("executionStats"), explainOutput);
            assert.eq(sbeNljStages.length, 0, explainOutput);
        }
    } else {
        assert.eq(lkpStages.length, 1, lkpStages);
        const lkpStage = lkpStages[0];
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
    }
};

let checkExplainOutputForAllVerbosityLevels = function(
    localColl, fromColl, expectedExplainResult, allowDiskUse, expectedQueryPlan = {}) {
    // The `explain` verbosity level: 'allPlansExecution'.
    let explainAllPlansOutput =
        explainAggregationLookup(localColl, fromColl, kAllPlansExecution, allowDiskUse);
    checkExplainOutputForVerLevel(
        explainAllPlansOutput, expectedExplainResult, kAllPlansExecution, expectedQueryPlan);

    // The `explain` verbosity level: 'executionStats'.
    let explainExecStatsOutput =
        explainAggregationLookup(localColl, fromColl, kExecutionStats, allowDiskUse);
    checkExplainOutputForVerLevel(
        explainExecStatsOutput, expectedExplainResult, kExecutionStats, expectedQueryPlan);

    // The `explain` verbosity level: 'queryPlanner'.
    let explainQueryPlannerOutput =
        explainAggregationLookup(localColl, fromColl, kQueryPlanner, allowDiskUse);
    checkExplainOutputForVerLevel(
        explainQueryPlannerOutput, expectedExplainResult, kQueryPlanner, expectedQueryPlan);

    // The `explain` verbosity level is not passed.
    let explainOutput = explainAggregationLookup(localColl, fromColl, {}, allowDiskUse);
    checkExplainOutputForVerLevel(explainOutput, expectedExplainResult, {}, expectedQueryPlan);
};

let testQueryExecutorStatsWithCollectionScan = function() {
    let output = doAggregationLookup(localColl, fromColl, {allowDiskUse: false});

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

    if (isSBELookupEnabled) {
        checkExplainOutputForAllVerbosityLevels(
            localColl,
            fromColl,
            {
                // When the SBE lookup is enabled, the execution stats can capture all the scanning
                // objects. So, totalDocsExmained must be same as
                // (total documents in local collection +
                //  total documents in local collection * total documents in foreign collection)
                // In this case: two documents in the local collection + one iteration over the
                // foreign collection for each document in the local collection (i.e., 2*10) = 22.
                totalDocsExamined: 2 + 2 * 10,
                totalKeysExamined: 0,
                // one scan over the local collection + one scan over the foreign collection for
                // each document in the local collection = 3.
                collectionScans: 1 + 2,
                collectionSeeks: 0,
                indexScans: 0,
                indexSeeks: 0,
                indexesUsed: []
            },
            {allowDiskUse: false},
            {strategy: "NestedLoopJoin"});
    } else {
        checkExplainOutputForAllVerbosityLevels(
            localColl,
            fromColl,
            {totalDocsExamined: 20, totalKeysExamined: 0, collectionScans: 4, indexesUsed: []},
            {allowDiskUse: false});
    }
};

let testQueryExecutorStatsWithHashLookup = function() {
    // "HashJoin" is available only in the SBE lookup.
    if (!isSBELookupEnabled) {
        return;
    }

    let output = doAggregationLookup(localColl, fromColl, {allowDiskUse: true});

    let expectedOutput = [
        {_id: 0, localField: 0, output: [{_id: 0, foreignField: 0}]},
        {_id: 1, localField: 1, output: [{_id: 1, foreignField: 1}]}
    ];

    assert.eq(output, expectedOutput);

    let [curScannedObjects, curScannedKeys] = getCurrentQueryExecutorStats();

    // For collection scan, total scannedObjects should be sum of
    // (total documents in local collection + total documents in foreign collection)
    assert.eq(localDocCount + foreignDocCount, curScannedObjects);

    // There is no index in the collection.
    assert.eq(0, curScannedKeys);

    checkExplainOutputForAllVerbosityLevels(
        localColl,
        fromColl,
        {
            // When the SBE lookup is enabled, the execution stats can capture all the scanning
            // objects. So, totalDocsExmained must be same as
            // (total documents in local collection + total documents in foreign collection).
            // In this case, we scan the foreign collection once (10 docs) to create the hash-table
            // and then scan the local collection (2 docs) once to check each document in the built
            // hash-table = 12.
            totalDocsExamined: 10 + 2,
            totalKeysExamined: 0,
            // scan the foreign collection once to create the hash-table and then scan the local
            // collection once to check each document in the built hash-table = 2.
            collectionScans: 1 + 1,
            collectionSeeks: 0,
            indexScans: 0,
            indexSeeks: 0,
            indexesUsed: []
        },
        {allowDiskUse: true},
        {strategy: "HashJoin"});
};

let createIndexForCollection = function(collection, fieldName) {
    let request = {};
    request[fieldName] = 1;
    assert.commandWorked(collection.createIndex(request));
};

let testQueryExecutorStatsWithIndexScan = function() {
    createIndexForCollection(fromColl, "foreignField");

    let output = doAggregationLookup(localColl, fromColl, {allowDiskUse: false});

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

    if (isSBELookupEnabled) {
        checkExplainOutputForAllVerbosityLevels(
            localColl,
            fromColl,
            {
                // When the SBE lookup is enabled, the execution stats can capture all the scanning
                // objects. So, totalDocsExmained must be same as
                // (total docs in local collection + total matched docs in foreign collection)
                totalDocsExamined: 2 + 2,
                // One index seek is done per each document in the local collection and one key is
                // examined per seek = 2.
                totalKeysExamined: 2,
                // The local collection get scanned = 1.
                collectionScans: 1,
                // For each examined key that matches in the index scan stage, a seek on foreign
                // collection is done to acquire the corresponding document in the foreign
                // collection = 2.
                collectionSeeks: 2,
                indexScans: 0,
                // One index seek is done per each document in the local collection = 2
                indexSeeks: 2,
                indexesUsed: ["foreignField_1"]
            },
            {allowDiskUse: false},
            {strategy: "IndexedLoopJoin", indexName: "foreignField_1"});
    } else {
        checkExplainOutputForAllVerbosityLevels(localColl,
                                                fromColl,
                                                {
                                                    totalDocsExamined: 2,
                                                    totalKeysExamined: 2,
                                                    collectionScans: 0,
                                                    indexesUsed: ["foreignField_1"]
                                                },
                                                {allowDiskUse: false});
    }
};

insertDocumentToCollection(fromColl, foreignDocCount, "foreignField");
insertDocumentToCollection(localColl, localDocCount, "localField");

// This test might be called over an existing MongoD instance. We should populate
// lastScannedObjects and lastScannedKeys with existing stats values in that case.
getCurrentQueryExecutorStats();

testQueryExecutorStatsWithCollectionScan();
testQueryExecutorStatsWithHashLookup();
testQueryExecutorStatsWithIndexScan();
}());
