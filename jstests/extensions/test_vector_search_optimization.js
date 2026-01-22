/**
 * Tests the $testVectorSearchOptimization stage extension in aggregation pipelines. This is used to confirm that the optimizations in document_source_extension_optimizable.cpp achieve parity between a native vectorSearch stage and a vectorSearch stage implemented as an extension.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   requires_fcv_82,
 * ]
 */

import {getAggPlanStages, getStageFromSplitPipeline} from "jstests/libs/query/analyze_plan.js";

const coll = db.test_vector_search_optimization;
coll.drop();

coll.insert([
    {_id: 1, x: 1},
    {_id: 2, x: 2},
    {_id: 3, x: 3},
]);

const buildTestVectorSearchOptStage = ({storedSource, ineligibleForSortOptimization}) => {
    return {
        $testVectorSearchOptimization: {
            storedSource: storedSource,
            ...(ineligibleForSortOptimization !== undefined && {ineligibleForSortOptimization}),
        },
    };
};

const desugarFalseStage = {$testVectorSearchOptimization: {desugar: false}};

////////////////////////////////////////////////////////////////////////////////////////////////////
// vectorSearch $sort Optimization Tests
////////////////////////////////////////////////////////////////////////////////////////////////////
const sortStageVectorSearchScore = {$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}}};
const sortStageScore = {$sort: {score: {$meta: "score"}}};

const verifySortOptimizationApplied = (
    stage,
    sortStages = [sortStageVectorSearchScore],
    shouldOptimize = true,
    interveningStages = [],
) => {
    const pipelineWithSort = [stage, ...interveningStages, ...sortStages];

    // Use explain to get the optimized pipeline structure
    const explainOutput = coll.explain("queryPlanner").aggregate(pipelineWithSort);
    // Check for $sort in both regular stages and splitPipeline (for sharded clusters)
    const sortFound =
        getStageFromSplitPipeline(explainOutput, "$sort") != null ||
        getAggPlanStages(explainOutput, "$sort").length > 0;

    if (shouldOptimize) {
        assert(!sortFound, "Expected $sort to be removed by optimization, but it was found in the explain output");
    } else {
        assert(sortFound, "Expected $sort to remain in pipeline, but it was not found in the explain output");
    }
};

const runTestWithDifferentStoredSourceVal = ({storedSource}) => {
    const validStage = buildTestVectorSearchOptStage({storedSource});

    // Standard case: single sort on 'vectorSearchScore' should be removed.
    verifySortOptimizationApplied(validStage);

    // Multiple consecutive sorts on vector search score directly after the desugared pipeline are optimized away.
    verifySortOptimizationApplied(validStage, [sortStageVectorSearchScore, sortStageVectorSearchScore]);

    // Three consecutive sorts should all be removed (edge case).
    verifySortOptimizationApplied(validStage, [
        sortStageVectorSearchScore,
        sortStageVectorSearchScore,
        sortStageVectorSearchScore,
    ]);

    // Verifies that a sort not on vector search score isn't optimized away.
    verifySortOptimizationApplied(validStage, [sortStageScore], false);

    // $sort with multi-field criteria on 'vectorSearchScore' and another field should not be removed.
    const multiFieldSort = {$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}, x: 1}};
    verifySortOptimizationApplied(validStage, [multiFieldSort], false);

    // Currently cannot optimize $sort that is not directly after $vectorSearch (with intervening stage).
    // TODO SERVER-96068: check that $sort is removed for these types of pipelines.
    verifySortOptimizationApplied(validStage, [sortStageVectorSearchScore], false, [{$limit: 67}]);
    verifySortOptimizationApplied(validStage, [sortStageVectorSearchScore], false, [{$addFields: {"cats": 67}}]);

    // Verify that an incorrectly desugared pipeline for vectorSearch (doesn't consist of the expected stages) does not undergo optimization.
    const invalidStage = buildTestVectorSearchOptStage({storedSource: false, ineligibleForSortOptimization: true});
    verifySortOptimizationApplied(invalidStage, [sortStageVectorSearchScore], false);
};

// Run optimizations when storedSource is false (desugared pipeline contains an $_internalSearchIdLookup stage).
runTestWithDifferentStoredSourceVal({storedSource: false});

// Run optimizations when storedSource is true (desugared pipeline contains an $replaceRoot stage).
runTestWithDifferentStoredSourceVal({storedSource: true});

// Test case where desugar is false - stage only expands to $testVectorSearch without idLookup/replaceRoot.
// The sort optimization should still work directly after $testVectorSearch.
const testDesugarFalse = () => {
    // Valid sort on vectorSearchScore should be optimized away
    verifySortOptimizationApplied(desugarFalseStage, [sortStageVectorSearchScore], true);

    // Invalid sort (not on vectorSearchScore) should NOT be optimized away
    verifySortOptimizationApplied(desugarFalseStage, [sortStageScore], false);
};

testDesugarFalse();

////////////////////////////////////////////////////////////////////////////////////////////////////
// vectorSearch $limit Optimization Tests
////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * Helper to get the extractedLimit value from $testVectorSearch stage in explain output
 */
const getExtractedLimit = (explainOutput) => {
    // Check splitPipeline first (sharded case)
    const vectorSearchInSplit = getStageFromSplitPipeline(explainOutput, "$testVectorSearch");
    if (vectorSearchInSplit && vectorSearchInSplit.$testVectorSearch) {
        return vectorSearchInSplit.$testVectorSearch.extractedLimit;
    }

    // Fall back to regular stages (non-sharded case)
    const vectorSearchStages = getAggPlanStages(explainOutput, "$testVectorSearch");
    if (vectorSearchStages.length > 0 && vectorSearchStages[0].$testVectorSearch) {
        return vectorSearchStages[0].$testVectorSearch.extractedLimit;
    }

    return undefined;
};

/**
 * Test limit extraction for vectorSearch optimization
 */
const testLimitExtraction = (stage) => {
    const testCases = [
        {
            name: "Single limit",
            stages: [{$limit: 10}],
            expectedLimit: 10,
        },
        {
            name: "Multiple limits (minimum extracted)",
            stages: [{$limit: 20}, {$limit: 5}, {$limit: 15}],
            expectedLimit: 5,
        },
        {
            name: "No limit",
            stages: [{$match: {x: 1}}],
            expectedLimit: undefined,
        },
        {
            name: "Skip and limit combined",
            stages: [{$skip: 10}, {$limit: 5}],
            expectedLimit: 15,
        },
        {
            name: "Limit after $project (should extract)",
            stages: [{$project: {x: 1}}, {$limit: 10}],
            expectedLimit: 10,
        },
        {
            name: "Limit after $unwind (blocking)",
            stages: [{$unwind: "$arr"}, {$limit: 10}],
            expectedLimit: undefined, // $unwind changes doc count
        },
        {
            name: "Limit with sort on vectorSearchScore",
            stages: [{$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}}}, {$limit: 10}],
            expectedLimit: 10,
        },
        {
            name: "Limit in middle of pipeline",
            stages: [{$project: {x: 1}}, {$limit: 15}, {$project: {x: 1}}],
            expectedLimit: 15,
        },
        {
            name: "Only skip, no limit",
            stages: [{$skip: 10}],
            expectedLimit: undefined,
        },
    ];

    testCases.forEach(({name, stages, expectedLimit}) => {
        const pipeline = [stage, ...stages];
        const explainOutput = coll.explain("queryPlanner").aggregate(pipeline);
        const extractedLimit = getExtractedLimit(explainOutput);
        assert.eq(
            extractedLimit,
            expectedLimit,
            `${name}: Expected limit ${expectedLimit}, got ${extractedLimit} for this pipeline: ${pipeline.map((stage) => stage.toString()).join(", ")}`,
        );
    });
};

// Run tests for both storedSource values
testLimitExtraction(buildTestVectorSearchOptStage({storedSource: false}));
testLimitExtraction(buildTestVectorSearchOptStage({storedSource: true}));

// Test limit extraction when desugar is false - stage only expands to $testVectorSearch
testLimitExtraction(desugarFalseStage);
