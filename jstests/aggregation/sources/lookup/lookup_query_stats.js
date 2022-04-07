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
 *     assumes_against_mongod_not_mongos
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");         // For 'getAggPlanStages'
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.
load("jstests/libs/sbe_explain_helpers.js");  // For getQueryInfoAtTopLevelOrFirstStage

const isSBELookupEnabled = checkSBEEnabled(db, ["featureFlagSBELookupPushdown"]);

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
    let lkpStages = getAggPlanStages(explainOutput, "$lookup");
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

        if (verbosityLevel && verbosityLevel !== kQueryPlanner) {
            assert(queryInfo.hasOwnProperty("executionStats"), explainOutput);

            const executionStats = queryInfo.executionStats;
            assert(executionStats.hasOwnProperty("totalDocsExamined"), executionStats);
            assert.eq(executionStats.totalDocsExamined, expected.totalDocsExamined, executionStats);
            assert(executionStats.hasOwnProperty("totalKeysExamined"), executionStats);
            assert.eq(executionStats.totalKeysExamined, expected.totalKeysExamined, executionStats);
        } else {  // If no `verbosityLevel` is passed or 'queryPlanner' is passed.
            assert(!queryInfo.hasOwnProperty("executionStats"), explainOutput);
        }
    } else {
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
        // When the SBE lookup is enabled, the execution stats can capture all the scanning
        // objects. So, totalDocsExmained must be same as
        // (total documents in local collection +
        //  total documents in local collection * total documents in foreign collection)
        checkExplainOutputForAllVerbosityLevels(localColl,
                                                fromColl,
                                                {totalDocsExamined: 22, totalKeysExamined: 0},
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

    // When the SBE lookup is enabled, the execution stats can capture all the scanning
    // objects. So, totalDocsExmained must be same as
    // (total documents in local collection + total documents in foreign collection)
    checkExplainOutputForAllVerbosityLevels(localColl,
                                            fromColl,
                                            {totalDocsExamined: 12, totalKeysExamined: 0},
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
        // When the SBE lookup is enabled, the execution stats can capture all the scanning
        // objects. So, totalDocsExmained must be same as
        // (total documents in local collection + total matched documents in foreign collection)
        checkExplainOutputForAllVerbosityLevels(
            localColl,
            fromColl,
            {totalDocsExamined: 4, totalKeysExamined: 2},
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
