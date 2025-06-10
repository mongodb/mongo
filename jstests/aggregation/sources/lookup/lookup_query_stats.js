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
 *     requires_pipeline_optimization,
 *     # During fcv upgrade/downgrade the engine might not be what we expect.
 *     cannot_run_during_upgrade_downgrade,
 * ]
 */
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {
    getQueryInfoAtTopLevelOrFirstStage,
    getSbePlanStages
} from "jstests/libs/query/sbe_explain_helpers.js";
import {
    checkSbeFullyEnabled,
    checkSbeRestrictedOrFullyEnabled
} from "jstests/libs/query/sbe_util.js";

const isSBEFullyEnabled = checkSbeFullyEnabled(db);
const isSBELookupEnabled = checkSbeRestrictedOrFullyEnabled(db);
const testDB = db.getSiblingDB("lookup_query_stats");
testDB.dropDatabase();

const localColl = testDB.getCollection("local");
const fromColl = testDB.getCollection("foreign");
const foreignDocCount = 10;
const localDocCount = 3;
const cities = ["New York", "Sydney", "Dublin", "London", "Palo Alto", "San Francisco"];

const kExecutionStats = "executionStats";
const kAllPlansExecution = "allPlansExecution";
const kQueryPlanner = "queryPlanner";

// Keeps track of the last query execution stats.
let lastScannedObjects = 0;
let lastScannedKeys = 0;

let insertCompatibleDocumentsToCollection = function(collection, docCount, fieldName) {
    const bulk = collection.initializeUnorderedBulkOp();
    for (let i = 0; i < docCount; i++) {
        let doc = {_id: i};
        doc[fieldName] = i;
        bulk.insert(doc);
    }
    assert.commandWorked(bulk.execute());
};

let insertMixedDocumentsToCollection = function(collection, docCount, fieldName) {
    const bulk = collection.initializeUnorderedBulkOp();
    for (let i = 0; i < docCount; i++) {
        let doc = {_id: i};
        if (i % 2 == 0) {
            doc[fieldName] = i;
        } else {
            doc[fieldName] = cities[i % cities.length];
        }
        bulk.insert(doc);
    }
    assert.commandWorked(bulk.execute());
};

let insertIncompatibleDocumentsToCollection = function(collection, docCount, fieldName) {
    const bulk = collection.initializeUnorderedBulkOp();
    for (let i = 0; i < docCount; i++) {
        let doc = {_id: i};
        doc[fieldName] = cities[i % cities.length];
        bulk.insert(doc);
    }
    assert.commandWorked(bulk.execute());
};

let aggregationLookupPipeline = function(localColl, fromColl, options, withUnwind) {
    const lookupStage =         {
            $lookup: {
                from: fromColl.getName(),
                localField: "localField",
                foreignField: "foreignField",
                as: "output"
            }
        };
    const sortStage = {$sort: {localField: 1}};
    const pipeline = withUnwind ? [lookupStage, {$unwind: {path: '$output'}}, sortStage]
                                : [lookupStage, sortStage];
    return localColl.aggregate(pipeline, options);
};

let doAggregationLookup = function(localColl, fromColl, options, withUnwind) {
    return aggregationLookupPipeline(localColl, fromColl, options, withUnwind).toArray();
};

let explainAggregationLookup = function(localColl, fromColl, verbosityLevel, options, withUnwind) {
    return aggregationLookupPipeline(
        localColl.explain(verbosityLevel), fromColl, options, withUnwind);
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
    explainOutput, expected, verbosityLevel, expectedQueryPlan, withUnwind) {
    // Only make SBE specific assertions when we know that our $lookup has been pushed down.
    if (isSBEFullyEnabled || (isSBELookupEnabled && !withUnwind)) {
        // If the SBE lookup is enabled, the "$lookup" stage is pushed down to the SBE and it's
        // not visible in 'stages' field of the explain output. Instead, 'queryPlan.stage' must be
        // "EQ_LOOKUP" or "EQ_LOOKUP_UNWIND".
        let lkpStages = getAggPlanStages(explainOutput, "EQ_LOOKUP", true);
        if (lkpStages.length == 0) {
            lkpStages = getAggPlanStages(explainOutput, "EQ_LOOKUP_UNWIND", true);
        }
        assert.eq(lkpStages.length, 1, lkpStages);
        const lkpStage = lkpStages[0];

        const queryInfo = getQueryInfoAtTopLevelOrFirstStage(explainOutput);
        const planner = queryInfo.queryPlanner;
        assert(planner.hasOwnProperty("winningPlan") &&
                   planner.winningPlan.hasOwnProperty("queryPlan"),
               explainOutput);
        const plan = planner.winningPlan.queryPlan;

        assert(lkpStage.hasOwnProperty("stage"), lkpStage);
        assert(lkpStage.stage == "EQ_LOOKUP" || lkpStage.stage == "EQ_LOOKUP_UNWIND", lkpStage);
        assert(expectedQueryPlan.hasOwnProperty("strategy"), expectedQueryPlan);
        assert(
            lkpStage.hasOwnProperty("strategy") && lkpStage.strategy == expectedQueryPlan.strategy,
            lkpStage);
        if (expectedQueryPlan.strategy == "IndexedLoopJoin" ||
            expectedQueryPlan.strategy === "DynamicIndexedLoopJoin") {
            assert(lkpStage.hasOwnProperty("indexName"), lkpStage);
            assert.eq(lkpStage.indexName, expectedQueryPlan.indexName);
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
        const lkpStages = getAggPlanStages(explainOutput, "$lookup");
        assert.eq(lkpStages.length, 1, lkpStages);
        const lkpStage = lkpStages[0];
        assert.eq(
            lkpStage.hasOwnProperty("$lookup") && lkpStage.$lookup.hasOwnProperty("unwinding"),
            withUnwind,
            lkpStage);
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
    localColl, fromColl, expectedExplainResult, allowDiskUse, withUnwind, expectedQueryPlan = {}) {
    // The `explain` verbosity level: 'allPlansExecution'.
    let explainAllPlansOutput =
        explainAggregationLookup(localColl, fromColl, kAllPlansExecution, allowDiskUse, withUnwind);
    checkExplainOutputForVerLevel(explainAllPlansOutput,
                                  expectedExplainResult,
                                  kAllPlansExecution,
                                  expectedQueryPlan,
                                  withUnwind);

    // The `explain` verbosity level: 'executionStats'.
    let explainExecStatsOutput =
        explainAggregationLookup(localColl, fromColl, kExecutionStats, allowDiskUse, withUnwind);
    checkExplainOutputForVerLevel(explainExecStatsOutput,
                                  expectedExplainResult,
                                  kExecutionStats,
                                  expectedQueryPlan,
                                  withUnwind);

    // The `explain` verbosity level: 'queryPlanner'.
    let explainQueryPlannerOutput =
        explainAggregationLookup(localColl, fromColl, kQueryPlanner, allowDiskUse, withUnwind);
    checkExplainOutputForVerLevel(explainQueryPlannerOutput,
                                  expectedExplainResult,
                                  kQueryPlanner,
                                  expectedQueryPlan,
                                  withUnwind);

    // The `explain` verbosity level is not passed.
    let explainOutput = explainAggregationLookup(localColl, fromColl, {}, allowDiskUse, withUnwind);
    checkExplainOutputForVerLevel(
        explainOutput, expectedExplainResult, {}, expectedQueryPlan, withUnwind);
};

let createIndexForCollection = function(collection, fieldName) {
    let request = {};
    request[fieldName] = 1;
    assert.commandWorked(collection.createIndex(request));
};

let testQueryExecutorStatsWithCollectionScan = function(params) {
    let output = doAggregationLookup(localColl, fromColl, {allowDiskUse: false}, params.withUnwind);

    assert.eq(output, params.expectedOutput);

    let [curScannedObjects, curScannedKeys] = getCurrentQueryExecutorStats();

    // For collection scan, total scannedObjects should be sum of
    // (total documents in local collection +
    //  total documents in local collection * total documents in foreign collection)
    const expectedScannedObjects = localDocCount + localDocCount * foreignDocCount;
    assert.eq(expectedScannedObjects, curScannedObjects);

    // There is no index in the collection.
    assert.eq(0, curScannedKeys);

    if (isSBEFullyEnabled || (isSBELookupEnabled && !params.withUnwind)) {
        checkExplainOutputForAllVerbosityLevels(
            localColl,
            fromColl,
            {
                // When the SBE lookup is enabled, the execution stats can capture all the scanning
                // objects. So, totalDocsExamined must be same as expectedScannedObjects.
                totalDocsExamined: expectedScannedObjects,
                totalKeysExamined: 0,
                // one scan over the local collection + one scan over the foreign collection for
                // each document in the local collection.
                collectionScans: 1 + localDocCount,
                collectionSeeks: 0,
                indexScans: 0,
                indexSeeks: 0,
                indexesUsed: []
            },
            {allowDiskUse: false},
            params.withUnwind,
            {strategy: "NestedLoopJoin"});
    } else {
        checkExplainOutputForAllVerbosityLevels(
            localColl,
            fromColl,
            {
                totalDocsExamined: localDocCount * foreignDocCount,
                totalKeysExamined: 0,
                collectionScans: localDocCount,
                indexesUsed: []
            },
            {allowDiskUse: false},
            params.withUnwind);
    }
};

let testQueryExecutorStatsWithHashLookup = function(params) {
    // "HashJoin" is available only in the SBE lookup.
    if (!isSBELookupEnabled) {
        return;
    }

    // SBE HashJoin doesn't $unwind internally.
    if (params.withUnwind) {
        return;
    }

    let options = {allowDiskUse: true};

    let output = doAggregationLookup(localColl, fromColl, options, params.withUnwind);

    assert.eq(output, params.expectedOutput);

    let [curScannedObjects, curScannedKeys] = getCurrentQueryExecutorStats();

    // For collection scan, total scannedObjects should be sum of
    // (total documents in local collection + total documents in foreign collection)
    const expectedScannedObjects = localDocCount + foreignDocCount;
    assert.eq(expectedScannedObjects, curScannedObjects);

    // There is no index in the collection.
    assert.eq(0, curScannedKeys);

    checkExplainOutputForAllVerbosityLevels(
        localColl,
        fromColl,
        {
            // When the SBE lookup is enabled, the execution stats can capture all the scanning
            // objects. So, totalDocsExamined must be same as expectedScannedObjects.
            totalDocsExamined: expectedScannedObjects,
            totalKeysExamined: 0,
            // scan the foreign collection once to create the hash-table and then scan the local
            // collection once to check each document in the built hash-table = 2.
            collectionScans: 1 + 1,
            collectionSeeks: 0,
            indexScans: 0,
            indexSeeks: 0,
            indexesUsed: []
        },
        options,
        params.withUnwind,
        {strategy: "HashJoin"});

    if (params.withIndex) {
        assert.commandWorked(fromColl.dropIndex({foreignField: 1}));
    }
};

let testQueryExecutorStatsWithIndexScan = function(params) {
    createIndexForCollection(fromColl, "foreignField");

    let output = doAggregationLookup(localColl, fromColl, {allowDiskUse: false}, params.withUnwind);

    assert.eq(output, params.expectedOutput);

    let [curScannedObjects, curScannedKeys] = getCurrentQueryExecutorStats();

    // The total number of scanned objects is #(documents from local collection that can use the
    // index) + #(documents from foreign collection that can match the index)
    const foreignDocMatchIndex = params.foreignMatchIndex;
    const expectedScannedObjects = localDocCount + foreignDocMatchIndex;

    assert.eq(expectedScannedObjects, curScannedObjects);

    // Number of keys scanned in the foreign collection should be equal to the number of keys that
    // match with the local collection
    assert.eq(foreignDocMatchIndex, curScannedKeys);

    if (isSBEFullyEnabled || (isSBELookupEnabled && !params.withUnwind)) {
        checkExplainOutputForAllVerbosityLevels(
            localColl,
            fromColl,
            {
                // When the SBE lookup is enabled, the execution stats can capture all the scanning
                // objects. So, totalDocsExamined must be same as expectedScannedObjects
                totalDocsExamined: expectedScannedObjects,
                // One index seek is done per each document in the local collection and one key is
                // examined per seek.
                totalKeysExamined: foreignDocMatchIndex,
                // The local collection get scanned 1 time and the foreign collection collection is
                // not scanned.
                collectionScans: 1,
                // For each examined key that matches in the index scan stage, a seek on foreign
                // collection is done to acquire the corresponding document in the foreign
                // collection.
                collectionSeeks: foreignDocMatchIndex,
                indexScans: 0,
                // One index seek is done per each document in the local collection that can use the
                // index
                indexSeeks: localDocCount,
                indexesUsed: ["foreignField_1"]
            },
            {allowDiskUse: false},
            params.withUnwind,
            {strategy: "IndexedLoopJoin", indexName: "foreignField_1"});
    } else {
        checkExplainOutputForAllVerbosityLevels(localColl,
                                                fromColl,
                                                {
                                                    totalDocsExamined: foreignDocMatchIndex,
                                                    totalKeysExamined: foreignDocMatchIndex,
                                                    collectionScans: 0,
                                                    indexesUsed: ["foreignField_1"]
                                                },
                                                {allowDiskUse: false},
                                                params.withUnwind);
    }

    assert.commandWorked(fromColl.dropIndex({foreignField: 1}));
};

let testQueryExecutorStatsWithDynamicIndexedLoopJoin = function(params) {
    createIndexForCollection(fromColl, "foreignField");

    let output = doAggregationLookup(localColl,
                                     fromColl,
                                     {allowDiskUse: params.allowDiskUse, collation: {locale: "fr"}},
                                     params.withUnwind);

    assert.eq(output, params.expectedOutput);

    let [curScannedObjects, curScannedKeys] = getCurrentQueryExecutorStats();

    // The total number of scanned objects is #(documents from local collection that can use the
    // index) + #(documents from foreign collection that can match the index)
    // #(documents from local collection that cannot use the index) + #(documents from local
    // collection that cannot use the index)*#(documents in foreign collection)
    const localDocCountNoIndex = params.localNoIndex;       // 1;
    const localDocCountIndex = params.localWithIndex;       // 2;
    const foreignDocMatchIndex = params.foreignMatchIndex;  //
    const expectedScannedObjects = localDocCountIndex + foreignDocMatchIndex +
        localDocCountNoIndex + localDocCountNoIndex * foreignDocCount;

    assert.eq(expectedScannedObjects, curScannedObjects);

    // Number of keys scanned in the foreign collection should equal the number of keys in foreign
    // collection that are match using an index
    assert.eq(foreignDocMatchIndex, curScannedKeys);

    if (isSBEFullyEnabled || (isSBELookupEnabled && !params.withUnwind)) {
        checkExplainOutputForAllVerbosityLevels(
            localColl,
            fromColl,
            {
                // When the SBE lookup is enabled, the execution stats can capture all the scanning
                // objects. So, totalDocsExamined must be same as expectedScannedObjects
                totalDocsExamined: expectedScannedObjects,
                // One index seek is done per each document in the local collection and one key is
                // examined per seek.
                totalKeysExamined: foreignDocMatchIndex,
                // The local collection get scanned 1 time and the foreign collection collection is
                // scanned once for each localDocCountNoIndex.
                collectionScans: 1 + localDocCountNoIndex,
                // For each examined key that matches in the index scan stage, a seek on foreign
                // collection is done to acquire the corresponding document in the foreign
                // collection.
                collectionSeeks: foreignDocMatchIndex,
                indexScans: 0,
                // One index seek is done per each document in the local collection that can use the
                // index
                indexSeeks: localDocCountIndex,
                indexesUsed: ["foreignField_1"]
            },
            {allowDiskUse: false, collation: {locale: "fr"}},
            params.withUnwind,
            {strategy: "DynamicIndexedLoopJoin", indexName: "foreignField_1"});
    } else {
        checkExplainOutputForAllVerbosityLevels(
            localColl,
            fromColl,
            {
                totalDocsExamined: localDocCountNoIndex * foreignDocCount + localDocCountIndex,
                totalKeysExamined: localDocCountIndex,
                collectionScans: localDocCountNoIndex,
                indexesUsed: localDocCountIndex ? ["foreignField_1"] : []
            },
            {allowDiskUse: false, collation: {locale: "fr"}},
            params.withUnwind);
    }

    assert.commandWorked(fromColl.dropIndex({foreignField: 1}));
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// TESTS
////////////////////////////////////////////////////////////////////////////////////////////////////

// Create collections with only objects that can use the index when the collation is incompatible.

insertCompatibleDocumentsToCollection(fromColl, foreignDocCount, "foreignField");
insertCompatibleDocumentsToCollection(localColl, localDocCount, "localField");

let expectedResults = [
    {
        results: [
            {"_id": 0, "localField": 0, "output": [{"_id": 0, "foreignField": 0}]},
            {"_id": 1, "localField": 1, "output": [{"_id": 1, "foreignField": 1}]},
            {"_id": 2, "localField": 2, "output": [{"_id": 2, "foreignField": 2}]}
        ],
        unwind: false
    },
    {
        results: [
            {"_id": 0, "localField": 0, "output": {"_id": 0, "foreignField": 0}},
            {"_id": 1, "localField": 1, "output": {"_id": 1, "foreignField": 1}},
            {"_id": 2, "localField": 2, "output": {"_id": 2, "foreignField": 2}}
        ],
        unwind: true
    }
];

// This test might be called over an existing MongoD instance. We should populate
// lastScannedObjects and lastScannedKeys with existing stats values in that case.
getCurrentQueryExecutorStats();

for (let res of expectedResults) {
    // No index, no disk usage allowed.
    testQueryExecutorStatsWithCollectionScan({withUnwind: res.unwind, expectedOutput: res.results});
    // No index, disk usage allowed.
    testQueryExecutorStatsWithHashLookup({withUnwind: res.unwind, expectedOutput: res.results});
    // Index with compatible collation.
    testQueryExecutorStatsWithIndexScan({
        withUnwind: res.unwind,
        expectedOutput: res.results,
        foreignMatchIndex: localDocCount,
    });
    // Index with incompatible collation, no disk usage allowed. The collation seems incompatible
    // but it is not. The statistics will be the same with the index statistics.
    testQueryExecutorStatsWithDynamicIndexedLoopJoin({
        withUnwind: res.unwind,
        expectedOutput: res.results,
        localNoIndex: 0,
        localWithIndex: localDocCount,
        foreignMatchIndex: localDocCount,
        allowDiskUse: false
    });
    // Index and incompatible collation, disk usage allowed. The collation seems incompatible but it
    // is not. The statistics will be the same with the index statistics.
    testQueryExecutorStatsWithDynamicIndexedLoopJoin({
        withUnwind: res.unwind,
        expectedOutput: res.results,
        localNoIndex: 0,
        localWithIndex: localDocCount,
        foreignMatchIndex: localDocCount,
        allowDiskUse: true
    });
}

// Create collections with objects that can use the index and objects that cannot when the collation
// is incompatible.

fromColl.drop();
localColl.drop();

insertMixedDocumentsToCollection(fromColl, foreignDocCount, "foreignField");
insertMixedDocumentsToCollection(localColl, localDocCount, "localField");

expectedResults = [
    {
        results: [
            {_id: 0, localField: 0, output: [{_id: 0, foreignField: 0}]},
            {_id: 2, localField: 2, output: [{_id: 2, foreignField: 2}]},
            {
                _id: 1,
                localField: "Sydney",
                "output":
                    [{"_id": 1, "foreignField": "Sydney"}, {"_id": 7, "foreignField": "Sydney"}]
            }
        ],
        unwind: false
    },
    {
        results: [
            {"_id": 0, "localField": 0, "output": {"_id": 0, "foreignField": 0}},
            {"_id": 2, "localField": 2, "output": {"_id": 2, "foreignField": 2}},
            {"_id": 1, "localField": "Sydney", "output": {"_id": 1, "foreignField": "Sydney"}},
            {"_id": 1, "localField": "Sydney", "output": {"_id": 7, "foreignField": "Sydney"}}
        ],
        unwind: true
    }
];

getCurrentQueryExecutorStats();

for (let res of expectedResults) {
    // No index, no disk usage allowed.
    testQueryExecutorStatsWithCollectionScan({withUnwind: res.unwind, expectedOutput: res.results});
    // No index, disk usage allowed.
    testQueryExecutorStatsWithHashLookup({withUnwind: res.unwind, expectedOutput: res.results});
    // Index with compatible collation.
    testQueryExecutorStatsWithIndexScan({
        withUnwind: res.unwind,
        expectedOutput: res.results,
        foreignMatchIndex: localDocCount + 1,
    });
    // Index with incompatible collation, no disk usage allowed.
    testQueryExecutorStatsWithDynamicIndexedLoopJoin({
        withUnwind: res.unwind,
        expectedOutput: res.results,
        localNoIndex: 1,
        localWithIndex: 2,
        foreignMatchIndex: 2,
        allowDiskUse: false
    });
    // Index with incompatible collation, disk usage allowed.
    testQueryExecutorStatsWithDynamicIndexedLoopJoin({
        withUnwind: res.unwind,
        expectedOutput: res.results,
        localNoIndex: 1,
        localWithIndex: 2,
        foreignMatchIndex: 2,
        allowDiskUse: true
    });
}

// Create collections with objects that cannot use the index when the collation is incompatible.

fromColl.drop();
localColl.drop();

insertIncompatibleDocumentsToCollection(fromColl, foreignDocCount, "foreignField");
insertIncompatibleDocumentsToCollection(localColl, localDocCount, "localField");

expectedResults = [
    {
        results: [
            {
                "_id": 2,
                "localField": "Dublin",
                "output":
                    [{"_id": 2, "foreignField": "Dublin"}, {"_id": 8, "foreignField": "Dublin"}]
            },
            {
                "_id": 0,
                "localField": "New York",
                "output":
                    [{"_id": 0, "foreignField": "New York"}, {"_id": 6, "foreignField": "New York"}]
            },
            {
                "_id": 1,
                "localField": "Sydney",
                "output":
                    [{"_id": 1, "foreignField": "Sydney"}, {"_id": 7, "foreignField": "Sydney"}]
            }

        ],
        unwind: false
    },
    {
        results: [
            {"_id": 2, "localField": "Dublin", "output": {"_id": 2, "foreignField": "Dublin"}},
            {"_id": 2, "localField": "Dublin", "output": {"_id": 8, "foreignField": "Dublin"}},
            {"_id": 0, "localField": "New York", "output": {"_id": 0, "foreignField": "New York"}},
            {"_id": 0, "localField": "New York", "output": {"_id": 6, "foreignField": "New York"}},
            {"_id": 1, "localField": "Sydney", "output": {"_id": 1, "foreignField": "Sydney"}},
            {"_id": 1, "localField": "Sydney", "output": {"_id": 7, "foreignField": "Sydney"}}
        ],
        unwind: true
    }
];

getCurrentQueryExecutorStats();

for (let res of expectedResults) {
    // No index, no disk usage allowed.
    testQueryExecutorStatsWithCollectionScan({withUnwind: res.unwind, expectedOutput: res.results});
    // No index, disk usage allowed.
    testQueryExecutorStatsWithHashLookup({withUnwind: res.unwind, expectedOutput: res.results});
    // Index with compatible collation.
    testQueryExecutorStatsWithIndexScan({
        withUnwind: res.unwind,
        expectedOutput: res.results,
        foreignMatchIndex: localDocCount + localDocCount,
    });
    // Index with incompatible collation, no disk usage allowed. None of the objects can use the
    // index so all objects will use scan.
    testQueryExecutorStatsWithDynamicIndexedLoopJoin({
        withUnwind: res.unwind,
        expectedOutput: res.results,
        localNoIndex: localDocCount,
        localWithIndex: 0,
        foreignMatchIndex: 0,
        allowDiskUse: false
    });
    // Index with incompatible collation, no disk usage allowed. None of the objects can use the
    // index so all objects will use scan.
    testQueryExecutorStatsWithDynamicIndexedLoopJoin({
        withUnwind: res.unwind,
        expectedOutput: res.results,
        localNoIndex: localDocCount,
        localWithIndex: 0,
        foreignMatchIndex: 0,
        allowDiskUse: true
    });
}
