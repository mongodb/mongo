/**
 * Tests the $testVectorSearchOptimization stage extension in aggregation pipelines. This is used to
 * confirm that the optimizations in document_source_extension_optimizable.cpp achieve parity
 * between a native vectorSearch stage and a vectorSearch stage implemented as an extension.
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
// getPipelineSuffixBounds Tests
//
// Each case verifies three values together:
//   - minBoundsType / maxBoundsType: the constraint kind returned by getPipelineSuffixBounds()
//   - extractedLimit: the discrete max value written via setExtractedLimitVal_deprecated(), which
//     is only set when maxBounds is discrete (this is the limit-pushdown path used for batch size
//     tuning; it subsumes the old standalone limit-extraction test coverage)
//
// desugarFalseStage is used so the suffix seen by $testVectorSearch is exactly the user-supplied
// stages. A separate block below exercises a representative subset with the storedSource variants
// ($replaceRoot, $_internalSearchIdLookup) to confirm those desugared stages do not affect bounds.
////////////////////////////////////////////////////////////////////////////////////////////////////

const getPipelineSuffixBoundsFromExplain = (explainOutput) => {
    const inSplit = getStageFromSplitPipeline(explainOutput, "$testVectorSearch");
    if (inSplit && inSplit.$testVectorSearch) {
        const {minBoundsType, maxBoundsType, extractedLimit} = inSplit.$testVectorSearch.limit;
        return {minBoundsType, maxBoundsType, extractedLimit};
    }
    const stages = getAggPlanStages(explainOutput, "$testVectorSearch");
    if (stages.length > 0 && stages[0].$testVectorSearch) {
        const {minBoundsType, maxBoundsType, extractedLimit} = stages[0].$testVectorSearch.limit;
        return {minBoundsType, maxBoundsType, extractedLimit};
    }
    return {minBoundsType: undefined, maxBoundsType: undefined, extractedLimit: undefined};
};

const assertPipelineSuffixBounds = (name, explainOutput, expected) => {
    const {minBoundsType, maxBoundsType, extractedLimit} = getPipelineSuffixBoundsFromExplain(explainOutput);
    assert.eq(
        minBoundsType,
        expected.expectedMinBoundsType,
        `${name}: unexpected minBoundsType. Full explain: ${tojson(explainOutput)}`,
    );
    assert.eq(
        maxBoundsType,
        expected.expectedMaxBoundsType,
        `${name}: unexpected maxBoundsType. Full explain: ${tojson(explainOutput)}`,
    );
    assert.eq(
        extractedLimit,
        expected.expectedExtractedLimit,
        `${name}: unexpected extractedLimit. Full explain: ${tojson(explainOutput)}`,
    );
};

const testPipelineSuffixBounds = () => {
    const testCases = [
        // ---- Unknown bounds ----
        {
            // No downstream stages: initial context has no constraints.
            name: "empty suffix",
            stages: [],
            expectedMinBoundsType: "unknownConstraints",
            expectedMaxBoundsType: "unknownConstraints",
            expectedExtractedLimit: undefined,
        },
        {
            // $match has unknown selectivity so the discrete max resets to Unknown.
            name: "$match only",
            stages: [{$match: {x: 1}}],
            expectedMinBoundsType: "unknownConstraints",
            expectedMaxBoundsType: "unknownConstraints",
            expectedExtractedLimit: undefined,
        },
        {
            // $skip with no $limit: skip only inflates existing discrete bounds, leaves Unknown unchanged.
            name: "$skip only (no limit)",
            stages: [{$skip: 10}],
            expectedMinBoundsType: "unknownConstraints",
            expectedMaxBoundsType: "unknownConstraints",
            expectedExtractedLimit: undefined,
        },
        {
            // $unwind may increase or decrease cardinality, so both min and max reset to Unknown.
            name: "$unwind before $limit",
            stages: [{$unwind: "$arr"}, {$limit: 10}],
            expectedMinBoundsType: "unknownConstraints",
            expectedMaxBoundsType: "unknownConstraints",
            expectedExtractedLimit: undefined,
        },
        // ---- NeedAll bounds ----
        {
            // $group is a blocking stage: needs all documents from its source.
            name: "$group (blocking)",
            stages: [{$group: {_id: "$x", count: {$sum: 1}}}],
            expectedMinBoundsType: "needAll",
            expectedMaxBoundsType: "needAll",
            expectedExtractedLimit: undefined,
        },
        {
            // $sort (non-metadata) is blocking: must consume all inputs even for top-k.
            name: "$sort on plain field (blocking)",
            stages: [{$sort: {x: 1}}],
            expectedMinBoundsType: "needAll",
            expectedMaxBoundsType: "needAll",
            expectedExtractedLimit: undefined,
        },
        // ---- Discrete bounds ----
        {
            // A plain $limit: both bounds collapse to discrete(N).
            name: "$limit only",
            stages: [{$limit: 10}],
            expectedMinBoundsType: "discrete",
            expectedMaxBoundsType: "discrete",
            expectedExtractedLimit: 10,
        },
        {
            // Multiple limits: the minimum across all limits is extracted.
            name: "multiple $limits (minimum wins)",
            stages: [{$limit: 20}, {$limit: 5}, {$limit: 15}],
            expectedMinBoundsType: "discrete",
            expectedMaxBoundsType: "discrete",
            expectedExtractedLimit: 5,
        },
        {
            // $skip inflates the limit: discrete bounds stay discrete (value = skip + limit).
            name: "$skip then $limit",
            stages: [{$skip: 5}, {$limit: 10}],
            expectedMinBoundsType: "discrete",
            expectedMaxBoundsType: "discrete",
            expectedExtractedLimit: 15,
        },
        {
            // Same as above with different values.
            name: "$skip then $limit (larger skip)",
            stages: [{$skip: 10}, {$limit: 5}],
            expectedMinBoundsType: "discrete",
            expectedMaxBoundsType: "discrete",
            expectedExtractedLimit: 15,
        },
        {
            // $project is a 1:1 transformation (DocumentSourceSingleDocumentTransformation):
            // no change to bounds; discrete limit propagates through.
            name: "$project then $limit",
            stages: [{$project: {x: 1}}, {$limit: 10}],
            expectedMinBoundsType: "discrete",
            expectedMaxBoundsType: "discrete",
            expectedExtractedLimit: 10,
        },
        {
            // $limit sandwiched between two $projects: both $projects are no-ops for bounds.
            name: "$project, $limit, $project",
            stages: [{$project: {x: 1}}, {$limit: 15}, {$project: {x: 1}}],
            expectedMinBoundsType: "discrete",
            expectedMaxBoundsType: "discrete",
            expectedExtractedLimit: 15,
        },
        {
            // The sort on vectorSearchScore is erased by the server optimizer (isSortedByVectorSearchScore)
            // before rule-based rewrites fire, so the effective suffix is just [$limit: 10].
            name: "$sort on vectorSearchScore then $limit (sort erased before bounds are computed)",
            stages: [{$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}}}, {$limit: 10}],
            expectedMinBoundsType: "discrete",
            expectedMaxBoundsType: "discrete",
            expectedExtractedLimit: 10,
        },
        // ---- Mixed bounds ----
        {
            // Reverse walk: $limit gives discrete(10)/discrete(10); $match resets max to Unknown
            // while leaving min at discrete(10).
            name: "$match before $limit (min=discrete, max=unknown)",
            stages: [{$match: {x: 1}}, {$limit: 10}],
            expectedMinBoundsType: "discrete",
            expectedMaxBoundsType: "unknownConstraints",
            expectedExtractedLimit: undefined,
        },
    ];

    testCases.forEach(({name, stages, expectedMinBoundsType, expectedMaxBoundsType, expectedExtractedLimit}) => {
        const pipeline = [desugarFalseStage, ...stages];
        const explainOutput = coll.explain("queryPlanner").aggregate(pipeline);
        assertPipelineSuffixBounds(name, explainOutput, {
            expectedMinBoundsType,
            expectedMaxBoundsType,
            expectedExtractedLimit,
        });
    });
};

testPipelineSuffixBounds();

// Verify that the stages introduced by desugaring ($replaceRoot for storedSource:true and
// $_internalSearchIdLookup for storedSource:false) do not perturb suffix bounds. We cannot rely
// purely on reading the visitor source for this — run a representative subset of cases with each
// desugared form.
{
    const storedSourceStage = buildTestVectorSearchOptStage({storedSource: true});
    const idLookupStage = buildTestVectorSearchOptStage({storedSource: false});

    const desugaredCases = [
        {
            name: "plain $limit is discrete through $replaceRoot",
            stage: storedSourceStage,
            suffix: [{$limit: 10}],
            expectedMinBoundsType: "discrete",
            expectedMaxBoundsType: "discrete",
            expectedExtractedLimit: 10,
        },
        {
            name: "$group (blocking) through $replaceRoot",
            stage: storedSourceStage,
            suffix: [{$group: {_id: "$x"}}],
            expectedMinBoundsType: "needAll",
            expectedMaxBoundsType: "needAll",
            expectedExtractedLimit: undefined,
        },
        {
            name: "plain $limit is discrete through $_internalSearchIdLookup",
            stage: idLookupStage,
            suffix: [{$limit: 10}],
            expectedMinBoundsType: "discrete",
            expectedMaxBoundsType: "discrete",
            expectedExtractedLimit: 10,
        },
        {
            name: "$match before $limit (mixed) through $_internalSearchIdLookup",
            stage: idLookupStage,
            suffix: [{$match: {x: 1}}, {$limit: 10}],
            expectedMinBoundsType: "discrete",
            expectedMaxBoundsType: "unknownConstraints",
            expectedExtractedLimit: undefined,
        },
    ];

    desugaredCases.forEach(
        ({name, stage, suffix, expectedMinBoundsType, expectedMaxBoundsType, expectedExtractedLimit}) => {
            const explainOutput = coll.explain("queryPlanner").aggregate([stage, ...suffix]);
            assertPipelineSuffixBounds(name, explainOutput, {
                expectedMinBoundsType,
                expectedMaxBoundsType,
                expectedExtractedLimit,
            });
        },
    );
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vectorSearch Rewrite Rule Optimization Tests
////////////////////////////////////////////////////////////////////////////////////////////////////

// Returns the inner spec of the $testVectorSearch stage from explain, handling both sharded
// (splitPipeline) and non-sharded topologies.
const getTestVectorSearchSpecFromExplain = (explainOutput) => {
    const inSplit = getStageFromSplitPipeline(explainOutput, "$testVectorSearch");
    if (inSplit && inSplit.$testVectorSearch) {
        return inSplit.$testVectorSearch;
    }
    const stages = getAggPlanStages(explainOutput, "$testVectorSearch");
    if (stages.length > 0 && stages[0].$testVectorSearch) {
        return stages[0].$testVectorSearch;
    }
    return undefined;
};

// Verify the in-place rule fired for desugarFalse: $testVectorSearch explain output should contain
// inPlaceRuleApplied: true.
{
    const explain = coll.explain("queryPlanner").aggregate([desugarFalseStage]);
    const stageSpec = getTestVectorSearchSpecFromExplain(explain);
    assert(stageSpec !== undefined, "Expected $testVectorSearch spec in explain: " + tojson(explain));
    assert.eq(
        stageSpec.inPlaceRuleApplied,
        true,
        "Expected inPlaceRuleApplied:true in explain, got: " + tojson(stageSpec),
    );
}

// Verify the in-place rule fires when the stage desugars (storedSource: true).
{
    const explain = coll.explain("queryPlanner").aggregate([buildTestVectorSearchOptStage({storedSource: true})]);
    const stageSpec = getTestVectorSearchSpecFromExplain(explain);
    assert(stageSpec !== undefined, "Expected $testVectorSearch spec in explain: " + tojson(explain));
    assert.eq(
        stageSpec.inPlaceRuleApplied,
        true,
        "Expected inPlaceRuleApplied:true for desugared (storedSource:true), got: " + tojson(stageSpec),
    );
}

// Verify the eraseStage rule fires: $project immediately following $testVectorSearch is removed.
{
    const pipeline = [desugarFalseStage, {$project: {_id: 1}}];
    const explain = coll.explain("queryPlanner").aggregate(pipeline);
    assert(
        !getStageFromSplitPipeline(explain, "$project"),
        "Expected $project to be erased by eraseStage rule, but found it in: " + tojson(explain),
    );
}

// Verify the eraseStage rule does NOT fire when the desugared stages intervene between
// $testVectorSearch and $project. With storedSource:false the pipeline after desugaring is
// [$testVectorSearch, $_internalSearchIdLookup, $project], so $project is not at nthNextStage(1).
{
    const pipeline = [buildTestVectorSearchOptStage({storedSource: false}), {$project: {_id: 1}}];
    const explain = coll.explain("queryPlanner").aggregate(pipeline);
    assert(
        getStageFromSplitPipeline(explain, "$project"),
        "Expected $project to remain since eraseStage should not fire with an intervening desugared " +
            "stage, but got: " +
            tojson(explain),
    );
}

// Verify the eraseStage rule successfully requeues and erases consecutive stages.
{
    const pipeline = [desugarFalseStage, {$project: {_id: 1}}, {$project: {_id: 1}}];
    const explain = coll.explain("queryPlanner").aggregate(pipeline);
    assert(
        !getStageFromSplitPipeline(explain, "$project"),
        "Expected $project to be erased by eraseStage rule, but found it in: " + tojson(explain),
    );
}

// Verify the eraseExtensionLimit rule fires: $extensionLimit two stages after $testVectorSearch is
// removed while the intervening $addFields stage is preserved.
{
    const pipeline = [desugarFalseStage, {$addFields: {a: 1}}, {$extensionLimit: 3}];
    const explain = coll.explain("queryPlanner").aggregate(pipeline);
    assert(
        !getStageFromSplitPipeline(explain, "$extensionLimit"),
        "Expected $extensionLimit to be erased by eraseExtensionLimit rule, but found it in: " + tojson(explain),
    );
    assert(
        getStageFromSplitPipeline(explain, "$addFields"),
        "Expected $addFields to remain after eraseExtensionLimit, but got: " + tojson(explain),
    );
}

// Verify eraseExtensionLimit fires for a desugared pipeline (storedSource:true). After desugaring,
// the pipeline is [$testVectorSearch, $replaceRoot, $extensionLimit], so $extensionLimit is at
// nthNextStage(2) and should be erased.
{
    const pipeline = [buildTestVectorSearchOptStage({storedSource: true}), {$extensionLimit: 3}];
    const explain = coll.explain("queryPlanner").aggregate(pipeline);
    assert(
        !getStageFromSplitPipeline(explain, "$extensionLimit"),
        "Expected $extensionLimit to be erased for desugared pipeline (storedSource:true), but " +
            "found it in: " +
            tojson(explain),
    );
}

// Verify the eraseExtensionLimit rule does NOT fire when stage two is not $extensionLimit.
{
    const pipeline = [desugarFalseStage, {$addFields: {a: 1}}, {$skip: 3}];
    const explain = coll.explain("queryPlanner").aggregate(pipeline);
    assert(
        getStageFromSplitPipeline(explain, "$skip"),
        "Expected $skip to remain since eraseExtensionLimit should not fire, but got: " + tojson(explain),
    );
}
