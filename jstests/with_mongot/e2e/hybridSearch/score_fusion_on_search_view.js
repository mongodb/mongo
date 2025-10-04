/**
 * Tests that $scoreFusion on a view namespace, defined with a search pipeline, is allowed and works
 * correctly.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_82]
 */

import {
    createHybridSearchPipeline,
    runHybridSearchOnSearchViewsTest,
    runHybridSearchWithAllMongotInputPipelinesOnSearchViewsTest,
    searchPipelineBar,
    searchPipelineFoo,
    vectorSearchPipelineV,
    vectorSearchPipelineZ,
} from "jstests/with_mongot/e2e_lib/hybrid_search_on_search_view.js";

/**
 * This function creates a $scoreFusion pipeline with the provided input pipelines.
 *
 * @param {object} inputPipelines spec for $scoreFusion input pipelines; can be as many as needed.
 */
const createScoreFusionPipeline = (inputPipelines, viewPipeline = null) => {
    const scoreFusionStage = {
        $scoreFusion: {input: {pipelines: {}, normalization: "sigmoid"}, combination: {method: "avg"}},
    };
    return createHybridSearchPipeline(inputPipelines, viewPipeline, scoreFusionStage, /**isRankFusion*/ false);
};

/**
 * This function creates a $scoreFusion pipeline with the provided input pipelines and runs it on
 * views defined with various search pipelines. Assumes that not all pipelines are mongot pipelines.
 *
 * @param {object} inputPipelines spec for $scoreFusion input pipelines; can be as many as
 *     needed.
 */
const runScoreFusionSearchViewsTest = (inputPipelines, checkCorrectness = true) => {
    runHybridSearchOnSearchViewsTest(inputPipelines, checkCorrectness, createScoreFusionPipeline);
};

/**
 * This function creates a $scoreFusion pipeline with the provided input pipelines and runs it on
 * views defined with various search pipelines. Asserts that the expected behavior is realized
 * when running aggregations on the view. Assuming that all input pipelines are search
 * pipelines, no search results should be returned so that's what's checked for.
 *
 * @param {object} inputPipelines spec for $scoreFusion input pipelines; can be as many as
 *     needed.
 */
const runScoreFusionWithAllMongotInputPipelinesOnSearchViewsTest = (inputPipelines) => {
    runHybridSearchWithAllMongotInputPipelinesOnSearchViewsTest(inputPipelines, createScoreFusionPipeline);
};

/* --------------------------------------------------------------------------------------- */
/* Run tests where $scoreFusion has NO mongot input pipelines. Should return results.*/

// score one pipeline
runScoreFusionSearchViewsTest({
    a: [{$score: {score: "$x", normalization: "minMaxScaler"}}, {$sort: {x: -1}}],
});
// score one pipeline with match
runScoreFusionSearchViewsTest({
    a: [
        {$match: {$expr: {$eq: ["$m", "bar"]}}},
        {$score: {score: "$x", normalization: "minMaxScaler"}},
        {$sort: {x: -1}},
    ],
});
// score two pipelines (reference collection fields)
runScoreFusionSearchViewsTest({
    a: [
        {$match: {$expr: {$eq: ["$m", "bar"]}}},
        {$score: {score: "$x", normalization: "minMaxScaler"}},
        {$sort: {x: -1}},
    ],
    b: [{$match: {$expr: {$eq: ["$a", "foo"]}}}, {$score: {score: "$y", normalization: "sigmoid"}}, {$sort: {x: 1}}],
});
// score two pipelines with matches (reference collection fields)
runScoreFusionSearchViewsTest({
    a: [{$score: {score: "$x", normalization: "minMaxScaler"}}, {$sort: {x: -1}}],
    b: [{$score: {score: "$y", normalization: "sigmoid"}}, {$sort: {x: 1}}],
});
// score and limit
runScoreFusionSearchViewsTest({
    a: [{$score: {score: "$y", normalization: "minMaxScaler"}}, {$sort: {x: 1}}, {$limit: 10}],
    b: [{$score: {score: "$x", normalization: "sigmoid"}}, {$sort: {x: -1}}, {$limit: 8}],
});
// $scores with embedded operators
runScoreFusionSearchViewsTest({
    a: [{$score: {score: {$add: ["$x", 4]}}}, {$sort: {x: 1}}],
    b: [{$score: {score: {$subtract: ["$x", 10]}}}, {$sort: {x: 1}}],
});
// only score
runScoreFusionSearchViewsTest({
    a: [{$score: {score: "$y", normalization: "minMaxScaler"}}],
    b: [{$score: {score: {$subtract: ["$x", 10]}}}],
});
// three pipelines
runScoreFusionSearchViewsTest({
    a: [
        {$match: {"$expr": {$gt: ["$y", 4]}}},
        {$score: {score: {$subtract: [4.0, 2]}, normalization: "sigmoid"}},
        {$sort: {x: 1}},
    ],
    b: [{$score: {score: {$subtract: [4.0, 2]}, normalization: "minMaxScaler"}}, {$sort: {x: 1}}],
    c: [
        {$match: {"$expr": {$lt: ["$x", 15]}}},
        {$score: {score: {$subtract: [4.0, 2]}, normalization: "sigmoid"}},
        {$sort: {x: -1}},
    ],
});
// limit in input
runScoreFusionSearchViewsTest(
    {
        a: [
            {$match: {x: {$gte: 4}}},
            {$score: {score: {$subtract: [100, "$y"]}, normalization: "sigmoid"}},
            {$sort: {x: 1}},
        ],
        b: [{$score: {score: "$x", normalization: "minMaxScaler"}}, {$limit: 5}, {$sort: {x: 1}}],
    },
    /*checkCorrectness=**/ false,
);
// sample
runScoreFusionSearchViewsTest(
    {
        a: [
            {$match: {x: {$gte: 4}}},
            {$score: {score: {$add: [10, 2]}, normalization: "minMaxScaler", weight: 0.5}},
            {$sort: {x: 1}},
        ],
        b: [{$score: {score: "$x", normalization: "sigmoid"}}, {$sample: {size: 5}}, {$sort: {x: 1}}],
    },
    /*checkCorrectness=**/ false,
);

/* --------------------------------------------------------------------------------------- */
/* Run tests where $scoreFusion has SOME mongot input pipelines. Should not return results that
 * reflect non-mongot input pipelines.
 */

// search first
runScoreFusionSearchViewsTest({
    a: [searchPipelineFoo],
    b: [{$score: {score: "$x", normalization: "minMaxScaler"}}],
});
// search second
runScoreFusionSearchViewsTest({
    a: [{$score: {score: "$x", normalization: "sigmoid"}}],
    b: [searchPipelineFoo],
});

// vector search first
runScoreFusionSearchViewsTest({
    a: [vectorSearchPipelineV],
    b: [{$score: {score: "$x", normalization: "minMaxScaler"}}],
});

// vector search second
runScoreFusionSearchViewsTest({
    a: [{$score: {score: "$x", normalization: "sigmoid"}}],
    b: [vectorSearchPipelineV],
});

// vector search second
runScoreFusionSearchViewsTest({
    a: [{$match: {"$expr": {$gt: ["$x", 4]}}}, {$score: {score: "$x", normalization: "sigmoid"}}],
    b: [vectorSearchPipelineV],
    c: [{$match: {"$expr": {$lte: ["$x", 45]}}}, {$score: {score: "$y", normalization: "minMaxScaler"}}],
    d: [searchPipelineBar],
});

/* --------------------------------------------------------------------------------------- */
/* Run tests where $scoreFusion has ONLY mongot input pipelines. Should not return any results.
 */

// only search
runScoreFusionWithAllMongotInputPipelinesOnSearchViewsTest({a: [searchPipelineFoo]});
// only vector search
runScoreFusionWithAllMongotInputPipelinesOnSearchViewsTest({a: [vectorSearchPipelineV]});
// double search
runScoreFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [searchPipelineFoo],
    b: [searchPipelineBar],
});
// swapped double search
runScoreFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [searchPipelineBar],
    b: [searchPipelineFoo],
});
// double vector search
runScoreFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [vectorSearchPipelineV],
    b: [vectorSearchPipelineZ],
});
// swapped double vector search
runScoreFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [vectorSearchPipelineZ],
    b: [vectorSearchPipelineV],
});
// multi search
runScoreFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [searchPipelineBar],
    b: [vectorSearchPipelineV],
});
// swapped multi search
runScoreFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [vectorSearchPipelineV],
    b: [searchPipelineBar],
});
