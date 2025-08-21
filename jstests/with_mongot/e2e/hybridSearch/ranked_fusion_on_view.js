/**
 * Tests that $rankFusion on a view namespace, defined with non-mongot and mongot pipelines, is
 * allowed and works correctly.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_82]
 */

import {
    createHybridSearchPipeline,
    runHybridSearchViewTest,
    searchPipelineBar,
    searchPipelineFoo,
    testHybridSearchViewWithSubsequentUnionOnDifferentView,
    testHybridSearchViewWithSubsequentUnionOnSameView,
    vectorSearchPipelineV,
    vectorSearchPipelineZ,
} from "jstests/with_mongot/e2e_lib/hybrid_search_on_view.js";

/**
 * This function creates a $rankFusion pipeline with the provided input pipelines. If a viewPipeline
 * is provided, it prepends the viewPipeline to each input pipeline.
 *
 * @param {object} inputPipelines spec for $rankFusion input pipelines; can be as many as needed.
 * @param {array}  viewPipeline pipeline to be prepended to the input pipelines.
 *                              If not provided, the input pipelines are used as is.
 */
export function createRankFusionPipeline(inputPipelines, viewPipeline = null) {
    const rankFusionStage = {$rankFusion: {input: {pipelines: {}}}};

    return createHybridSearchPipeline(inputPipelines, viewPipeline, rankFusionStage);
}

/**
 * This function creates a view with the provided name and pipeline, runs a $rankFusion against the
 * view, then runs the same $rankFusion against the main collection with the view stage prepended to
 * the input pipelines in the same way that the view desugaring works. It compares both the results
 * and the explain output of both queries to ensure they are the same.
 *
 * @param {string} testName name of the test, used to create a unique view.
 * @param {object} inputPipelines spec for $rankFusion input pipelines; can be as many as needed.
 * @param {array}  viewPipeline pipeline to create a view and to be prepended manually to the input
 *                              pipelines.
 * @param {bool}   checkCorrectness some pipelines are able to parse and run, but the order of the
 *                                  stages or the stages themselves are non-deterministic, so we
 *                                  can't check for correctness.
 * @param {bool}   isMongotPipeline true if the input pipelines are mongot pipelines (contains a
 *     search or vectorSearch stage). Use this to determine whether to compare explain results since
 *     they will otherwise differ if $rankFusion has a hybrid search input pipeline.
 */
const runRankFusionViewTest = (testName, inputPipelines, viewPipeline, checkCorrectness, isMongotPipeline = false) => {
    runHybridSearchViewTest(
        testName,
        inputPipelines,
        viewPipeline,
        checkCorrectness,
        isMongotPipeline,
        createRankFusionPipeline,
    );
};

(function testRankFusionViewCasesWhereFirstViewStageMustBeFirstStageInPipeline() {
    runRankFusionViewTest(
        "geo_near",
        {
            a: [{$match: {x: {$gt: 3}}}, {$sort: {x: -1}}],
            b: [{$match: {x: {$lte: 15}}}, {$sort: {x: 1}}],
        },
        [{$geoNear: {spherical: true, near: {type: "Point", coordinates: [1, 1]}}}],
        /*checkCorrectness=**/ true,
    );

    runRankFusionViewTest(
        "match_with_text",
        {
            a: [{$match: {x: {$gt: 3}}}, {$sort: {x: -1}}],
            b: [{$match: {x: {$lte: 15}}}, {$sort: {x: 1}}],
        },
        [{$match: {$text: {$search: "foo"}}}],
        /*CheckCorrectness=**/ true,
    );
})();

// Excluded tests:
// - $geoNear can't run against views.
(function testRankFusionViewSimpleViews() {
    runRankFusionViewTest(
        "simple_match",
        {
            a: [{$match: {x: {$gt: 5}}}, {$sort: {x: -1}}],
            b: [{$match: {x: {$lte: 15}}}, {$sort: {x: 1}}],
        },
        [{$match: {a: "foo"}}],
        /*checkCorrectness=**/ true,
    );
    runRankFusionViewTest(
        "view_produces_no_results",
        {
            a: [{$match: {x: {$gt: 5}}}, {$sort: {x: -1}}],
            b: [{$match: {x: {$lte: 15}}}, {$sort: {x: 1}}],
        },
        [{$match: {a: "adithi"}}],
        /*checkCorrectness=**/ true,
    );
    runRankFusionViewTest(
        "match_and_limit",
        {
            a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
            b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}],
        },
        [{$match: {a: "bar"}}],
        /*checkCorrectness=**/ true,
    );
    runRankFusionViewTest(
        "limit_in_view",
        {
            a: [{$match: {x: {$gte: 4}}}, {$sort: {x: 1}}],
            b: [{$match: {x: {$lt: 10}}}, {$sort: {x: 1}}],
        },
        [{$sort: {x: -1}}, {$limit: 15}],
        /*checkCorrectness=**/ true,
    );
    runRankFusionViewTest(
        "no_match",
        {
            a: [{$sort: {x: 1}}],
            b: [{$sort: {x: -1}}],
        },
        [{$sort: {x: -1}}, {$limit: 15}],
        /*checkCorrectness=**/ true,
    );
    runRankFusionViewTest(
        "three_pipelines",
        {
            a: [{$match: {a: "foo"}}, {$sort: {x: 1}}],
            b: [{$match: {a: "bar"}}, {$sort: {x: 1}}],
            c: [{$match: {x: {$lt: 10}}}, {$sort: {x: -1}}],
        },
        [{$match: {"$expr": {$gt: ["$x", 2]}}}],
        /*checkCorrectness=**/ true,
    );
    runRankFusionViewTest(
        "limit_in_input",
        {
            a: [{$match: {x: {$gte: 4}}}, {$sort: {x: 1}}],
            b: [{$limit: 5}, {$sort: {x: 1}}],
        },
        [{$match: {"$expr": {$lt: ["$x", 10]}}}],
        /*checkCorrectness=**/ false,
    );
    runRankFusionViewTest(
        "sample",
        {
            a: [{$match: {x: {$gte: 4}}}, {$sort: {x: 1}}],
            b: [{$sample: {size: 5}}, {$sort: {x: 1}}],
        },
        [{$match: {"$expr": {$lt: ["$x", 10]}}}],
        /*checkCorrectness=**/ false,
    );
})();

(function testRankFusionViewSearchViews() {
    runRankFusionViewTest(
        "only_search",
        {a: [searchPipelineFoo]},
        [{$match: {"$expr": {$lt: ["$x", 0]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true,
    );
    runRankFusionViewTest(
        "search_first",
        {
            a: [searchPipelineFoo],
            b: [{$sort: {x: 1}}],
        },
        [{$match: {$expr: {$eq: ["$a", "bar"]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true,
    );
    runRankFusionViewTest(
        "search_second",
        {
            a: [{$sort: {x: -1}}],
            b: [searchPipelineFoo],
        },
        [{$match: {"$expr": {$lt: ["$x", 0]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true,
    );
    runRankFusionViewTest(
        "only_vector_search",
        {a: [vectorSearchPipelineV]},
        [{$match: {$expr: {$eq: ["$a", "foo"]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true,
    );
    runRankFusionViewTest(
        "vector_search_first",
        {
            a: [vectorSearchPipelineV],
            b: [{$sort: {x: 1}}],
        },
        [{$match: {$expr: {$eq: ["$a", "foo"]}}}],
        /*checkCorrectness=**/ false,
        /*isMongotPipeline=**/ true,
    );
    runRankFusionViewTest(
        "vector_search_second",
        {
            a: [{$sort: {x: -1}}],
            b: [vectorSearchPipelineV],
        },
        [{$match: {$expr: {$eq: ["$a", "foo"]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true,
    );
    runRankFusionViewTest(
        "double_search",
        {
            a: [searchPipelineFoo],
            b: [searchPipelineBar],
        },
        [{$match: {"$expr": {$lt: ["$x", 10]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true,
    );
    runRankFusionViewTest(
        "swapped_double_search",
        {
            a: [searchPipelineBar],
            b: [searchPipelineFoo],
        },
        [{$match: {"$expr": {$lt: ["$x", 10]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true,
    );
    runRankFusionViewTest(
        "double_vector_search",
        {
            a: [vectorSearchPipelineV],
            b: [vectorSearchPipelineZ],
        },
        [{$match: {"$expr": {$lt: ["$x", 10]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true,
    );
    runRankFusionViewTest(
        "swapped_double_vector_search",
        {
            a: [vectorSearchPipelineZ],
            b: [vectorSearchPipelineV],
        },
        [{$match: {"$expr": {$lt: ["$x", 10]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true,
    );
    runRankFusionViewTest(
        "multi_search",
        {
            a: [searchPipelineBar],
            b: [vectorSearchPipelineV],
        },
        [{$match: {"$expr": {$lt: ["$x", 10]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true,
    );
    runRankFusionViewTest(
        "swapped_multi_search",
        {
            a: [vectorSearchPipelineV],
            b: [searchPipelineBar],
        },
        [{$match: {"$expr": {$lt: ["$x", 10]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true,
    );
})();

// Test a $unionWith following a $rankFusion to verify that the $rankFusion desugaring doesn't
// interfere with view resolution of the user provided $unionWith.
(function testRankFusionViewWithSubsequentUnionOnSameView() {
    const rankFusionInputPipelines = {
        a: [{$match: {x: {$gt: 3}}}, {$sort: {x: -1}}],
        b: [{$match: {x: {$lte: 15}}}, {$sort: {x: 1}}],
    };

    testHybridSearchViewWithSubsequentUnionOnSameView(rankFusionInputPipelines, createRankFusionPipeline);
})();

// Test a $unionWith following a $rankFusion to verify that the $rankFusion desugaring doesn't
// interfere with view resolution of the user provided $unionWith.
(function testRankFusionViewWithSubsequentUnionOnDifferentView() {
    const rankFusionInputPipelines = {
        a: [{$match: {x: {$gt: 3}}}, {$sort: {x: -1}}],
        b: [{$match: {x: {$lte: 15}}}, {$sort: {x: 1}}],
    };
    testHybridSearchViewWithSubsequentUnionOnDifferentView(rankFusionInputPipelines, createRankFusionPipeline);
})();
