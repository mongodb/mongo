/**
 * Tests that $scoreFusion on a view namespace, defined with non-mongot and mongot pipelines, is
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
    vectorSearchPipelineZ
} from "jstests/with_mongot/e2e_lib/hybrid_search_on_view.js";

/**
 * This function creates a $scoreFusion pipeline with the provided input pipelines. If a
 * viewPipeline is provided, it prepends the viewPipeline to each input pipeline.
 *
 * @param {object} inputPipelines spec for $scoreFusion input pipelines; can be as many as needed.
 * @param {array}  viewPipeline pipeline to be prepended to the input pipelines.
 *                              If not provided, the input pipelines are used as is.
 */
export function createScoreFusionPipeline(inputPipelines, viewPipeline = null) {
    const scoreFusionStage = {
        $scoreFusion:
            {input: {pipelines: {}, normalization: "sigmoid"}, combination: {method: "avg"}}
    };

    return createHybridSearchPipeline(
        inputPipelines, viewPipeline, scoreFusionStage, /**isRankFusion*/ false);
}

/**
 * This function creates a view with the provided name and pipeline, runs a $scoreFusion against the
 * view, then runs the same $scoreFusion against the main collection with the view stage prepended
 * to the input pipelines in the same way that the view desugaring works. It compares both the
 * results and the explain output of both queries to ensure they are the same.
 *
 * @param {string} testName name of the test, used to create a unique view.
 * @param {object} inputPipelines spec for $scoreFusion input pipelines; can be as many as needed.
 * @param {array}  viewPipeline pipeline to create a view and to be prepended manually to the input
 *                              pipelines.
 * @param {bool}   checkCorrectness some pipelines are able to parse and run, but the order of the
 *                                  stages or the stages themselves are non-deterministic, so we
 *                                  can't check for correctness.
 * @param {bool}   isMongotPipeline true if the input pipelines are mongot pipelines (contains a
 *     search or vectorSearch stage). Use this to determine whether to compare explain results since
 *     they will otherwise differ if $scoreFusion has a hybrid search input pipeline.
 */
const runScoreFusionViewTest =
    (testName, inputPipelines, viewPipeline, checkCorrectness, isMongotPipeline = false) => {
        runHybridSearchViewTest(testName,
                                inputPipelines,
                                viewPipeline,
                                checkCorrectness,
                                isMongotPipeline,
                                createScoreFusionPipeline);
    };

(function testScoreFusionViewCasesWhereFirstViewStageMustBeFirstStageInPipeline() {
    runScoreFusionViewTest(
        "geo_near",
        {
            a: [
                {$score: {score: "$x", normalization: "minMaxScaler"}},
                {$match: {x: {$gt: 3}}},
                {$sort: {x: -1}}
            ],
            b: [
                {$score: {score: "$y", normalization: "sigmoid"}},
                {$match: {x: {$lte: 15}}},
                {$sort: {x: 1}}
            ],
        },
        [{$geoNear: {spherical: true, near: {type: "Point", coordinates: [1, 1]}}}],
        /*checkCorrectness=**/ true);

    runScoreFusionViewTest("match_with_text",
                           {
                               a: [
                                   {$score: {score: "$x", normalization: "minMaxScaler"}},
                                   {$match: {x: {$gt: 3}}},
                                   {$sort: {x: -1}}
                               ],
                               b: [
                                   {$score: {score: "$y", normalization: "sigmoid"}},
                                   {$match: {x: {$lte: 15}}},
                                   {$sort: {x: 1}}
                               ],
                           },
                           [{$match: {$text: {$search: "foo"}}}],
                           /*CheckCorrectness=**/ true);
})();

// TODO SERVER-105677: Add tests for $skip.
// Excluded tests:
// - $geoNear can't run against views.
(function testScoreFusionViewSimpleViews() {
    runScoreFusionViewTest("only_score_match_view",
                           {
                               a: [{$score: {score: "$x", normalization: "minMaxScaler"}}],
                               b: [{$score: {score: "$y", normalization: "sigmoid"}}],
                           },
                           [{$match: {a: "foo"}}],
                           /*checkCorrectness=**/ true);
    runScoreFusionViewTest(
        "simple_score",
        {
            a: [{$score: {score: "$x", normalization: "minMaxScaler"}}, {$sort: {x: -1}}],
            b: [{$score: {score: "$y", normalization: "sigmoid"}}, {$sort: {x: 1}}],
        },
        [{$match: {a: "foo"}}],
        /*checkCorrectness=**/ true);
    runScoreFusionViewTest(
        "simple_score",
        {
            a: [
                {$match: {$expr: {$eq: ["$m", "bar"]}}},
                {$score: {score: "$x", normalization: "minMaxScaler"}},
                {$sort: {x: -1}}
            ],
            b: [{$score: {score: "$y", normalization: "sigmoid"}}, {$sort: {x: 1}}],
        },
        [{$match: {a: "foo"}}],
        /*checkCorrectness=**/ true);
    runScoreFusionViewTest(
        "view_produces_no_results",
        {
            a: [{$score: {score: "$x", normalization: "minMaxScaler"}}, {$sort: {x: -1}}],
            b: [{$score: {score: "$y", normalization: "sigmoid"}}, {$sort: {x: 1}}],
        },
        [{$match: {a: "adithi"}}],
        /*checkCorrectness=**/ true);
    runScoreFusionViewTest(
        "score_and_limit",
        {
            a: [
                {$score: {score: "$y", normalization: "minMaxScaler"}},
                {$sort: {x: 1}},
                {$limit: 10}
            ],
            b: [{$score: {score: "$x", normalization: "sigmoid"}}, {$sort: {x: -1}}, {$limit: 8}],
        },
        [{$match: {a: "bar"}}],
        /*checkCorrectness=**/ true);
    runScoreFusionViewTest("limit_in_view",
                           {
                               a: [{$score: {score: {$add: ["$x", 4]}}}, {$sort: {x: 1}}],
                               b: [{$score: {score: {$subtract: ["$x", 10]}}}, {$sort: {x: 1}}],
                           },
                           [{$sort: {x: -1}}, {$limit: 15}],
                           /*checkCorrectness=**/ true);
    runScoreFusionViewTest("only_score",
                           {
                               a: [{$score: {score: "$y", normalization: "minMaxScaler"}}],
                               b: [{$score: {score: {$subtract: ["$x", 10]}}}],
                           },
                           [{$sort: {x: -1}}, {$limit: 15}],
                           /*checkCorrectness=**/ true);
    runScoreFusionViewTest(
        "three_pipelines",
        {
            a: [
                {$match: {"$expr": {$gt: ["$y", 4]}}},
                {$score: {score: {$subtract: [4.0, 2]}, normalization: "sigmoid"}},
                {$sort: {x: 1}}
            ],
            b: [
                {$score: {score: {$subtract: [4.0, 2]}, normalization: "minMaxScaler"}},
                {$sort: {x: 1}}
            ],
            c: [
                {$match: {"$expr": {$lt: ["$x", 15]}}},
                {$score: {score: {$subtract: [4.0, 2]}, normalization: "sigmoid"}},
                {$sort: {x: -1}}
            ],
        },
        [{$match: {"$expr": {$gt: ["$x", 2]}}}],
        /*checkCorrectness=**/ true);
    runScoreFusionViewTest(
        "limit_in_input",
        {
            a: [
                {$match: {x: {$gte: 4}}},
                {$score: {score: {$subtract: [100, "$y"]}, normalization: "minMaxScaler"}},
                {$sort: {x: 1}}
            ],
            b: [{$score: {score: "$x", normalization: "sigmoid"}}, {$limit: 5}, {$sort: {x: 1}}],
        },
        [{$match: {"$expr": {$lt: ["$x", 10]}}}],
        /*checkCorrectness=**/ false);
    runScoreFusionViewTest(
        "sample",
        {
            a: [
                {$match: {x: {$gte: 4}}},
                {$score: {score: {$add: [10, 2]}, normalization: "minMaxScaler", weight: 0.5}},
                {$sort: {x: 1}}
            ],
            b: [
                {$score: {score: '$x', normalization: "sigmoid"}},
                {$sample: {size: 5}},
                {$sort: {x: 1}}
            ],
        },
        [{$match: {"$expr": {$lt: ["$x", 10]}}}],
        /*checkCorrectness=**/ false);
})();

(function testScoreFusionViewSearchViews() {
    runScoreFusionViewTest("only_search",
                           {a: [searchPipelineFoo]},
                           [{$match: {"$expr": {$lt: ["$x", 0]}}}],
                           /*checkCorrectness=**/ true,
                           /*isMongotPipeline=**/ true);
    runScoreFusionViewTest("search_first",
                           {
                               a: [searchPipelineFoo],
                               b: [{$score: {score: '$x', normalization: "minMaxScaler"}}],
                           },
                           [{$match: {$expr: {$eq: ["$a", "bar"]}}}],
                           /*checkCorrectness=**/ true,
                           /*isMongotPipeline=**/ true);
    runScoreFusionViewTest(
        "search_followed_by_score_first",
        {
            a: [searchPipelineFoo, {$score: {score: '$y', normalization: "sigmoid"}}],
            b: [{$score: {score: '$x', normalization: "minMaxScaler"}}],
        },
        [{$match: {$expr: {$eq: ["$a", "bar"]}}}],
        /*checkCorrectness=**/ true,
        /*isMongotPipeline=**/ true);
    runScoreFusionViewTest("search_second",
                           {
                               a: [{$score: {score: '$x', normalization: "sigmoid"}}],
                               b: [searchPipelineFoo],
                           },
                           [{$match: {"$expr": {$lt: ["$x", 0]}}}],
                           /*checkCorrectness=**/ true,
                           /*isMongotPipeline=**/ true);
    runScoreFusionViewTest("only_vector_search",
                           {a: [vectorSearchPipelineV]},
                           [{$match: {$expr: {$eq: ["$a", "foo"]}}}],
                           /*checkCorrectness=**/ true,
                           /*isMongotPipeline=**/ true);
    runScoreFusionViewTest("vector_search_first",
                           {
                               a: [vectorSearchPipelineV],
                               b: [{$score: {score: '$x', normalization: "minMaxScaler"}}],
                           },
                           [{$match: {$expr: {$eq: ["$a", "foo"]}}}],
                           /*checkCorrectness=**/ false,
                           /*isMongotPipeline=**/ true);
    runScoreFusionViewTest("vector_search_second",
                           {
                               a: [{$score: {score: '$x', normalization: "sigmoid"}}],
                               b: [vectorSearchPipelineV],
                           },
                           [{$match: {$expr: {$eq: ["$a", "foo"]}}}],
                           /*checkCorrectness=**/ true,
                           /*isMongotPipeline=**/ true);
    runScoreFusionViewTest("double_search",
                           {
                               a: [searchPipelineFoo],
                               b: [searchPipelineBar],
                           },
                           [{$match: {"$expr": {$lt: ["$x", 10]}}}],
                           /*checkCorrectness=**/ true,
                           /*isMongotPipeline=**/ true);
    runScoreFusionViewTest("swapped_double_search",
                           {
                               a: [searchPipelineBar],
                               b: [searchPipelineFoo],
                           },
                           [{$match: {"$expr": {$lt: ["$x", 10]}}}],
                           /*checkCorrectness=**/ true,
                           /*isMongotPipeline=**/ true);
    runScoreFusionViewTest("double_vector_search",
                           {
                               a: [vectorSearchPipelineV],
                               b: [vectorSearchPipelineZ],
                           },
                           [{$match: {"$expr": {$lt: ["$x", 10]}}}],
                           /*checkCorrectness=**/ true,
                           /*isMongotPipeline=**/ true);
    runScoreFusionViewTest("swapped_double_vector_search",
                           {
                               a: [vectorSearchPipelineZ],
                               b: [vectorSearchPipelineV],
                           },
                           [{$match: {"$expr": {$lt: ["$x", 10]}}}],
                           /*checkCorrectness=**/ true,
                           /*isMongotPipeline=**/ true);
    runScoreFusionViewTest("multi_search",
                           {
                               a: [searchPipelineBar],
                               b: [vectorSearchPipelineV],
                           },
                           [{$match: {"$expr": {$lt: ["$x", 10]}}}],
                           /*checkCorrectness=**/ true,
                           /*isMongotPipeline=**/ true);
    runScoreFusionViewTest("swapped_multi_search",
                           {
                               a: [vectorSearchPipelineV],
                               b: [searchPipelineBar],
                           },
                           [{$match: {"$expr": {$lt: ["$x", 10]}}}],
                           /*checkCorrectness=**/ true,
                           /*isMongotPipeline=**/ true);
})();

// Test a $unionWith following a $scoreFusion to verify that the $scoreFusion desugaring doesn't
// interfere with view resolution of the user provided $unionWith.
(function testScoreFusionViewWithSubsequentUnionOnSameView() {
    const scoreFusionInputPipelines = {
        a: [
            {$score: {score: "$x", normalization: "minMaxScaler"}},
            {$match: {x: {$gt: 3}}},
            {$sort: {x: -1}}
        ],
        b: [
            {$score: {score: "$y", normalization: "sigmoid"}},
            {$match: {x: {$lte: 15}}},
            {$sort: {x: 1}}
        ],
    };

    testHybridSearchViewWithSubsequentUnionOnSameView(scoreFusionInputPipelines,
                                                      createScoreFusionPipeline);
})();

// Test a $unionWith following a $scoreFusion to verify that the $scoreFusion desugaring doesn't
// interfere with view resolution of the user provided $unionWith.
(function testScoreFusionViewWithSubsequentUnionOnDifferentView() {
    const scoreFusionInputPipelines = {
        a: [
            {$score: {score: "$x", normalization: "minMaxScaler"}},
            {$match: {x: {$gt: 3}}},
            {$sort: {x: -1}}
        ],
        b: [
            {$score: {score: "$y", normalization: "sigmoid"}},
            {$match: {x: {$lte: 15}}},
            {$sort: {x: 1}}
        ],
    };
    testHybridSearchViewWithSubsequentUnionOnDifferentView(scoreFusionInputPipelines,
                                                           createScoreFusionPipeline);
})();
