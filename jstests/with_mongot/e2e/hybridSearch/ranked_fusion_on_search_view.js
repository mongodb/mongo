/**
 * Tests that $rankFusion on a view namespace, defined with a search pipeline, is allowed and works
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
 * This function creates a $rankFusion pipeline with the provided input pipelines.
 *
 * @param {object} inputPipelines spec for $rankFusion input pipelines; can be as many as needed.
 */
const createRankFusionPipeline = (inputPipelines, viewPipeline = null) => {
    const rankFusionStage = {$rankFusion: {input: {pipelines: {}}}};
    return createHybridSearchPipeline(
        inputPipelines, viewPipeline, rankFusionStage, /**isRankFusion*/ true);
};

/**
 * This function creates a $rankFusion pipeline with the provided input pipelines and runs it on
 * views defined with various search pipelines. Assumes that not all pipelines are mongot pipelines.
 *
 * @param {object} inputPipelines spec for $rankFusion input pipelines; can be as many as
 *     needed.
 */
const runRankFusionSearchViewsTest = (inputPipelines, checkCorrectness = true) => {
    runHybridSearchOnSearchViewsTest(inputPipelines, checkCorrectness, createRankFusionPipeline);
};

/**
 * This function creates a $rankFusion pipeline with the provided input pipelines and runs it on
 * views defined with various search pipelines. Asserts that the expected behavior is realized
 * when running aggregations on the view. Assuming that all input pipelines are search
 * pipelines, no search results should be returned so that's what's checked for.
 *
 * @param {object} inputPipelines spec for $rankFusion input pipelines; can be as many as
 *     needed.
 */
const runRankFusionWithAllMongotInputPipelinesOnSearchViewsTest = (inputPipelines) => {
    runHybridSearchWithAllMongotInputPipelinesOnSearchViewsTest(inputPipelines,
                                                                createRankFusionPipeline);
};

/* --------------------------------------------------------------------------------------- */
/* Run tests where $rankFusion has NO mongot input pipelines. Should return results.*/

// match one pipeline
runRankFusionSearchViewsTest({
    a: [{$match: {x: {$gt: 5}}}, {$sort: {x: -1}}],
});
// match two pipelines
runRankFusionSearchViewsTest({
    a: [{$match: {x: {$gt: 5}}}, {$sort: {x: -1}}],
    b: [{$match: {x: {$lte: 15}}}, {$sort: {x: 1}}],
});
// match and limit
runRankFusionSearchViewsTest({
    a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
    b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}],
});
// limit in view
runRankFusionSearchViewsTest({
    a: [{$match: {x: {$gte: 4}}}, {$sort: {x: 1}}],
    b: [{$match: {x: {$lt: 10}}}, {$sort: {x: 1}}],
});
// no match
runRankFusionSearchViewsTest({
    a: [{$sort: {x: 1}}],
    b: [{$sort: {x: -1}}],
});
// three pipelines
runRankFusionSearchViewsTest({
    a: [{$match: {a: "foo"}}, {$sort: {x: 1}}],
    b: [{$match: {a: "bar"}}, {$sort: {x: 1}}],
    c: [{$match: {x: {$lt: 10}}}, {$sort: {x: -1}}],
});
// limit in input
runRankFusionSearchViewsTest({
    a: [{$match: {x: {$gte: 4}}}, {$sort: {x: 1}}],
    b: [{$limit: 5}, {$sort: {x: 1}}],
},
                             /*checkCorrectness=**/ false);
// sample
runRankFusionSearchViewsTest({
    a: [{$match: {x: {$gte: 4}}}, {$sort: {x: 1}}],
    b: [{$sample: {size: 5}}, {$sort: {x: 1}}],
},
                             /*checkCorrectness=**/ false);

/* --------------------------------------------------------------------------------------- */
/* Run tests where $rankFusion has SOME mongot input pipelines. Should not return results that
 * reflect non-mongot input pipelines.
 */

// search first
runRankFusionSearchViewsTest(
    {
        a: [searchPipelineFoo],
        b: [{$sort: {x: 1}}],
    },
);
// search second
runRankFusionSearchViewsTest(
    {
        a: [{$sort: {x: -1}}],
        b: [searchPipelineFoo],
    },
);

// vector search first
runRankFusionSearchViewsTest(
    {
        a: [vectorSearchPipelineV],
        b: [{$sort: {x: 1}}],
    },
);

// vector search second
runRankFusionSearchViewsTest(
    {
        a: [{$sort: {x: -1}}],
        b: [vectorSearchPipelineV],
    },
);

/* --------------------------------------------------------------------------------------- */
/* Run tests where $rankFusion has ONLY mongot input pipelines. Should not return any results.
 */

// only search
runRankFusionWithAllMongotInputPipelinesOnSearchViewsTest({a: [searchPipelineFoo]});
// only vector search
runRankFusionWithAllMongotInputPipelinesOnSearchViewsTest({a: [vectorSearchPipelineV]});
// double search
runRankFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [searchPipelineFoo],
    b: [searchPipelineBar],
});
// swapped double search
runRankFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [searchPipelineBar],
    b: [searchPipelineFoo],
});
// double vector search
runRankFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [vectorSearchPipelineV],
    b: [vectorSearchPipelineZ],
});
// swapped double vector search
runRankFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [vectorSearchPipelineZ],
    b: [vectorSearchPipelineV],
});
// multi search
runRankFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [searchPipelineBar],
    b: [vectorSearchPipelineV],
});
// swapped multi search
runRankFusionWithAllMongotInputPipelinesOnSearchViewsTest({
    a: [vectorSearchPipelineV],
    b: [searchPipelineBar],
});
