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

const buildTestVectorSearchOptStage = ({storedSource}) => {
    return {$testVectorSearchOptimization: {storedSource}};
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

    const explainOutput = coll.explain("queryPlanner").aggregate(pipelineWithSort);
    // Check for $sort in both regular stages and splitPipeline (for sharded clusters).
    const sortFound =
        getStageFromSplitPipeline(explainOutput, "$sort") != null ||
        getAggPlanStages(explainOutput, "$sort").length > 0;

    if (shouldOptimize) {
        assert(
            !sortFound,
            "Expected $sort to be removed by optimization, but it was found in the explain output",
        );
    } else {
        assert(
            sortFound,
            "Expected $sort to remain in pipeline, but it was not found in the explain output",
        );
    }
};

// storedSource=false desugars to [$testVectorSearch, $_internalSearchIdLookup, ...].
// $_internalSearchIdLookup already sets preservesOrderAndMetadata=true, so REDUNDANT_SORT_REMOVAL
// walks past it and finds $testVectorSearch's sort pattern and removes the $sort.
const runTestStoredSourceFalse = () => {
    const validStage = buildTestVectorSearchOptStage({storedSource: false});
    const multiFieldSort = {$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}, x: 1}};

    verifySortOptimizationApplied(validStage);
    verifySortOptimizationApplied(validStage, [
        sortStageVectorSearchScore,
        sortStageVectorSearchScore,
    ]);
    verifySortOptimizationApplied(validStage, [
        sortStageVectorSearchScore,
        sortStageVectorSearchScore,
        sortStageVectorSearchScore,
    ]);
    verifySortOptimizationApplied(validStage, [sortStageScore], false);
    verifySortOptimizationApplied(validStage, [multiFieldSort], false);
    // TODO SERVER-127594: check that $sort is removed for these types of pipelines with intervening
    // stages that preserve order and metadata.
    verifySortOptimizationApplied(validStage, [sortStageVectorSearchScore], false, [{$limit: 67}]);
    verifySortOptimizationApplied(validStage, [sortStageVectorSearchScore], false, [
        {$addFields: {"cats": 67}},
    ]);
};

// storedSource=true desugars to [$testVectorSearch, $replaceRoot, ...].
// $replaceRoot sets preservesOrderAndMetadata=true, so the backward walk continues past it and
// finds $testVectorSearch's sort pattern, removing the redundant $sort. Other intervening stages
// (e.g. $addFields) do not yet set preservesOrderAndMetadata=true, so the walk stops there.
const runTestStoredSourceTrue = () => {
    const validStage = buildTestVectorSearchOptStage({storedSource: true});
    const multiFieldSort = {$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}, x: 1}};

    verifySortOptimizationApplied(validStage, [sortStageVectorSearchScore], true);
    verifySortOptimizationApplied(
        validStage,
        [sortStageVectorSearchScore, sortStageVectorSearchScore],
        true,
    );
    verifySortOptimizationApplied(
        validStage,
        [sortStageVectorSearchScore, sortStageVectorSearchScore, sortStageVectorSearchScore],
        true,
    );
    verifySortOptimizationApplied(validStage, [sortStageScore], false);
    verifySortOptimizationApplied(validStage, [multiFieldSort], false);
    // TODO SERVER-127594: check that $sort is removed for these types of pipelines with intervening
    // stages that preserve order and metadata.
    verifySortOptimizationApplied(validStage, [sortStageVectorSearchScore], false, [{$limit: 67}]);
    verifySortOptimizationApplied(validStage, [sortStageVectorSearchScore], false, [
        {$addFields: {"cats": 67}},
    ]);
};

runTestStoredSourceFalse();
runTestStoredSourceTrue();

// desugar=false: stage expands only to $testVectorSearch (no intervening idLookup/replaceRoot).
// REDUNDANT_SORT_REMOVAL walks directly to $testVectorSearch's sort pattern.
const testDesugarFalse = () => {
    verifySortOptimizationApplied(desugarFalseStage, [sortStageVectorSearchScore], true);
    verifySortOptimizationApplied(desugarFalseStage, [sortStageScore], false);
};

testDesugarFalse();

////////////////////////////////////////////////////////////////////////////////////////////////////
// getPipelineSuffixBounds Tests
//
// These tests run with featureFlagExtensionsOptimizations ON (via --runAllFeatureFlagTests). The
// applyPipelineBounds rule calls getPipelineSuffixBounds() and $testVectorSearch stores the derived
// limit in pipelineBoundsLimit. The host does NOT call setExtractedLimitVal_deprecated() (the old
// way extensions extracted limits) when the flag is on, so extractedLimit is always absent.
//
// Each case verifies:
//   - minBoundsType / maxBoundsType: constraint type as returned from getPipelineSuffixBounds()
//   - pipelineBoundsLimit: discrete max value derived by the extension rule (flag ON / new path)
//   - extractedLimit: always undefined here (flag ON; host does not push via deprecated path)
//
// Limit extraction correctness for the deprecated path is covered in a separate dedicated test.
// desugarFalseStage is used so the suffix seen by $testVectorSearch is exactly the user-supplied
// stages. A separate block below exercises a representative subset with the storedSource variants
// ($replaceRoot, $_internalSearchIdLookup) to confirm those desugared stages do not affect bounds.
////////////////////////////////////////////////////////////////////////////////////////////////////

const getPipelineSuffixBoundsFromExplain = (explainOutput) => {
    const inSplit = getStageFromSplitPipeline(explainOutput, "$testVectorSearch");
    if (inSplit && inSplit.$testVectorSearch) {
        const {minBoundsType, maxBoundsType, extractedLimit, pipelineBoundsLimit} =
            inSplit.$testVectorSearch.limit;
        return {minBoundsType, maxBoundsType, extractedLimit, pipelineBoundsLimit};
    }
    const stages = getAggPlanStages(explainOutput, "$testVectorSearch");
    if (stages.length > 0 && stages[0].$testVectorSearch) {
        const {minBoundsType, maxBoundsType, extractedLimit, pipelineBoundsLimit} =
            stages[0].$testVectorSearch.limit;
        return {minBoundsType, maxBoundsType, extractedLimit, pipelineBoundsLimit};
    }
    // Return extractedLimit so later checks correctly assert the old limit extraction path was not
    // exercised.
    return {
        minBoundsType: undefined,
        maxBoundsType: undefined,
        extractedLimit: undefined,
        pipelineBoundsLimit: undefined,
    };
};

const assertPipelineSuffixBounds = (name, explainOutput, expected) => {
    const {minBoundsType, maxBoundsType, extractedLimit, pipelineBoundsLimit} =
        getPipelineSuffixBoundsFromExplain(explainOutput);
    assert.eq(
        minBoundsType,
        expected.minBoundsType,
        `${name}: unexpected minBoundsType. Full explain: ${tojson(explainOutput)}`,
    );
    assert.eq(
        maxBoundsType,
        expected.maxBoundsType,
        `${name}: unexpected maxBoundsType. Full explain: ${tojson(explainOutput)}`,
    );
    // extractedLimit is always absent here: flag is ON so the host does not push via deprecated
    // path.
    assert.eq(
        extractedLimit,
        undefined,
        `${name}: extractedLimit must be absent when flag is ON. Full explain: ${tojson(explainOutput)}`,
    );
    assert.eq(
        pipelineBoundsLimit,
        expected.pipelineBoundsLimit,
        `${name}: unexpected pipelineBoundsLimit. Full explain: ${tojson(explainOutput)}`,
    );
};

const testPipelineSuffixBounds = () => {
    const testCases = [
        // ---- Unknown bounds ----
        {
            // No downstream stages: initial context has no constraints.
            name: "empty suffix",
            stages: [],
            minBoundsType: "unknownConstraints",
            maxBoundsType: "unknownConstraints",
            pipelineBoundsLimit: undefined,
        },
        {
            // $match has unknown selectivity so the discrete max resets to Unknown.
            name: "$match only",
            stages: [{$match: {x: 1}}],
            minBoundsType: "unknownConstraints",
            maxBoundsType: "unknownConstraints",
            pipelineBoundsLimit: undefined,
        },
        {
            // $skip with no $limit: skip only inflates existing discrete bounds, leaves Unknown unchanged.
            name: "$skip only (no limit)",
            stages: [{$skip: 10}],
            minBoundsType: "unknownConstraints",
            maxBoundsType: "unknownConstraints",
            pipelineBoundsLimit: undefined,
        },
        {
            // $unwind may increase or decrease cardinality, so both min and max reset to Unknown.
            name: "$unwind before $limit",
            stages: [{$unwind: "$arr"}, {$limit: 10}],
            minBoundsType: "unknownConstraints",
            maxBoundsType: "unknownConstraints",
            pipelineBoundsLimit: undefined,
        },
        // ---- NeedAll bounds ----
        {
            // $group is a blocking stage: needs all documents from its source.
            name: "$group (blocking)",
            stages: [{$group: {_id: "$x", count: {$sum: 1}}}],
            minBoundsType: "needAll",
            maxBoundsType: "needAll",
            pipelineBoundsLimit: undefined,
        },
        {
            // $sort (non-metadata) is blocking: must consume all inputs even for top-k.
            name: "$sort on plain field (blocking)",
            stages: [{$sort: {x: 1}}],
            minBoundsType: "needAll",
            maxBoundsType: "needAll",
            pipelineBoundsLimit: undefined,
        },
        // ---- Discrete bounds ----
        {
            // A plain $limit: both bounds collapse to discrete(N).
            name: "$limit only",
            stages: [{$limit: 10}],
            minBoundsType: "discrete",
            maxBoundsType: "discrete",
            pipelineBoundsLimit: 10,
        },
        {
            // Multiple limits: the minimum across all limits is extracted.
            name: "multiple $limits (minimum wins)",
            stages: [{$limit: 20}, {$limit: 5}, {$limit: 15}],
            minBoundsType: "discrete",
            maxBoundsType: "discrete",
            pipelineBoundsLimit: 5,
        },
        {
            // $skip inflates the limit: discrete bounds stay discrete (value = skip + limit).
            name: "$skip then $limit",
            stages: [{$skip: 5}, {$limit: 10}],
            minBoundsType: "discrete",
            maxBoundsType: "discrete",
            pipelineBoundsLimit: 15,
        },
        {
            // Same as above with different values.
            name: "$skip then $limit (larger skip)",
            stages: [{$skip: 10}, {$limit: 5}],
            minBoundsType: "discrete",
            maxBoundsType: "discrete",
            pipelineBoundsLimit: 15,
        },
        {
            // $project is a 1:1 transformation (DocumentSourceSingleDocumentTransformation):
            // no change to bounds; discrete limit propagates through.
            name: "$project then $limit",
            stages: [{$project: {x: 1}}, {$limit: 10}],
            minBoundsType: "discrete",
            maxBoundsType: "discrete",
            pipelineBoundsLimit: 10,
        },
        {
            // $limit sandwiched between two $projects: both $projects are no-ops for bounds.
            name: "$project, $limit, $project",
            stages: [{$project: {x: 1}}, {$limit: 15}, {$project: {x: 1}}],
            minBoundsType: "discrete",
            maxBoundsType: "discrete",
            pipelineBoundsLimit: 15,
        },
        {
            // The sort on vectorSearchScore is erased by REDUNDANT_SORT_REMOVAL before rule-based
            // rewrites fire, so the effective suffix seen by getPipelineSuffixBounds() is just
            // [$limit: 10].
            name: "$sort on vectorSearchScore then $limit (sort erased before bounds are computed)",
            stages: [{$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}}}, {$limit: 10}],
            minBoundsType: "discrete",
            maxBoundsType: "discrete",
            pipelineBoundsLimit: 10,
        },
        // ---- Mixed bounds ----
        {
            // Reverse walk: $limit gives discrete(10)/discrete(10); $match resets max to Unknown
            // while leaving min at discrete(10).
            name: "$match before $limit (min=discrete, max=unknown)",
            stages: [{$match: {x: 1}}, {$limit: 10}],
            minBoundsType: "discrete",
            maxBoundsType: "unknownConstraints",
            pipelineBoundsLimit: undefined,
        },
    ];

    testCases.forEach(({name, stages, minBoundsType, maxBoundsType, pipelineBoundsLimit}) => {
        const pipeline = [desugarFalseStage, ...stages];
        const explainOutput = coll.explain("queryPlanner").aggregate(pipeline);
        assertPipelineSuffixBounds(name, explainOutput, {
            minBoundsType,
            maxBoundsType,
            pipelineBoundsLimit,
        });
    });
};

testPipelineSuffixBounds();

// Verify that the stages introduced by desugaring ($replaceRoot for storedSource:true and
// $_internalSearchIdLookup for storedSource:false) do not affect suffix bounds.
{
    const storedSourceStage = buildTestVectorSearchOptStage({storedSource: true});
    const idLookupStage = buildTestVectorSearchOptStage({storedSource: false});

    const desugaredCases = [
        {
            name: "plain $limit is discrete through $replaceRoot",
            stage: storedSourceStage,
            suffix: [{$limit: 10}],
            minBoundsType: "discrete",
            maxBoundsType: "discrete",
            pipelineBoundsLimit: 10,
        },
        {
            name: "$group (blocking) through $replaceRoot",
            stage: storedSourceStage,
            suffix: [{$group: {_id: "$x"}}],
            minBoundsType: "needAll",
            maxBoundsType: "needAll",
            pipelineBoundsLimit: undefined,
        },
        {
            name: "plain $limit is discrete through $_internalSearchIdLookup",
            stage: idLookupStage,
            suffix: [{$limit: 10}],
            minBoundsType: "discrete",
            maxBoundsType: "discrete",
            pipelineBoundsLimit: 10,
        },
        {
            name: "$match before $limit (mixed) through $_internalSearchIdLookup",
            stage: idLookupStage,
            suffix: [{$match: {x: 1}}, {$limit: 10}],
            minBoundsType: "discrete",
            maxBoundsType: "unknownConstraints",
            pipelineBoundsLimit: undefined,
        },
    ];

    desugaredCases.forEach(
        ({name, stage, suffix, minBoundsType, maxBoundsType, pipelineBoundsLimit}) => {
            const explainOutput = coll.explain("queryPlanner").aggregate([stage, ...suffix]);
            assertPipelineSuffixBounds(name, explainOutput, {
                minBoundsType,
                maxBoundsType,
                pipelineBoundsLimit,
            });
        },
    );
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vectorSearch $limit Optimization Tests
//
// Tests run with featureFlagExtensionsOptimizations ON (via --runAllFeatureFlagTests). The
// applyPipelineBounds rule fires and stores the result in pipelineBoundsLimit. extractedLimit
// (the deprecated path) is verified absent in every case.
// Tests run for all three stage variants: storedSource:false, storedSource:true, desugar:false.
//
// Coverage for the deprecated path (featureFlagExtensionsOptimizations OFF, extractedLimit set)
// lives in jstests/noPassthrough/extensions/vector_search_limit_deprecated_path.js.
////////////////////////////////////////////////////////////////////////////////////////////////////

const testLimitOptimization = (stage) => {
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
            name: "No limit (match only)",
            stages: [{$match: {x: 1}}],
            expectedLimit: undefined,
        },
        {
            name: "Skip and limit combined",
            stages: [{$skip: 10}, {$limit: 5}],
            expectedLimit: 15,
        },
        {
            name: "Limit after $project (transparent)",
            stages: [{$project: {x: 1}}, {$limit: 10}],
            expectedLimit: 10,
        },
        {
            name: "Limit after $unwind (blocking)",
            stages: [{$unwind: "$arr"}, {$limit: 10}],
            expectedLimit: undefined,
        },
        {
            name: "Limit with sort on vectorSearchScore (sort erased before limit is computed)",
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
        const {extractedLimit, pipelineBoundsLimit} =
            getPipelineSuffixBoundsFromExplain(explainOutput);
        assert.eq(
            pipelineBoundsLimit,
            expectedLimit,
            `${name} (stage ${tojson(stage)}): unexpected pipelineBoundsLimit`,
            {explainOutput},
        );
        assert.eq(
            extractedLimit,
            undefined,
            `${name} (stage ${tojson(stage)}): extractedLimit must be absent`,
            {explainOutput},
        );
    });
};

testLimitOptimization(buildTestVectorSearchOptStage({storedSource: false}));
testLimitOptimization(buildTestVectorSearchOptStage({storedSource: true}));
testLimitOptimization(desugarFalseStage);

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
    assert(
        stageSpec !== undefined,
        "Expected $testVectorSearch spec in explain: " + tojson(explain),
    );
    assert.eq(
        stageSpec.inPlaceRuleApplied,
        true,
        "Expected inPlaceRuleApplied:true in explain, got: " + tojson(stageSpec),
    );
}

// Verify the in-place rule fires when the stage desugars (storedSource: true).
{
    const explain = coll
        .explain("queryPlanner")
        .aggregate([buildTestVectorSearchOptStage({storedSource: true})]);
    const stageSpec = getTestVectorSearchSpecFromExplain(explain);
    assert(
        stageSpec !== undefined,
        "Expected $testVectorSearch spec in explain: " + tojson(explain),
    );
    assert.eq(
        stageSpec.inPlaceRuleApplied,
        true,
        "Expected inPlaceRuleApplied:true for desugared (storedSource:true), got: " +
            tojson(stageSpec),
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
        "Expected $extensionLimit to be erased by eraseExtensionLimit rule, but found it in: " +
            tojson(explain),
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

// Verify eraseExtensionLimit fires for storedSource:false. After desugaring,
// the pipeline is [$testVectorSearch, $_internalSearchIdLookup, $extensionLimit],
// so $extensionLimit is at nthNextStage(2) and should be erased.
{
    const pipeline = [buildTestVectorSearchOptStage({storedSource: false}), {$extensionLimit: 3}];
    const explain = coll.explain("queryPlanner").aggregate(pipeline);
    assert(
        !getStageFromSplitPipeline(explain, "$extensionLimit"),
        "Expected $extensionLimit to be erased for desugared pipeline (storedSource:false), but " +
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
        "Expected $skip to remain since eraseExtensionLimit should not fire, but got: " +
            tojson(explain),
    );
}
