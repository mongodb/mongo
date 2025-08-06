/*
 * This test verifies that the $scoreFusion stage correctly handles multiple scoring stages and
 * scoreDetails produces the expected results. It achieves this by verifying that the last score
 * producing stage of each $scoreFusion input pipeline is the one that determines said input
 * pipeline's score and scoreDetails.
 *
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_82 ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e_lib/data/movies.js";

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));
// Index is blocking by default so that the query is only run after index has been made.
createSearchIndex(coll, getMovieSearchIndexSpec());

// Create vector search index on movie plot embeddings.
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

const limit = 20;
// Multiplication factor of limit for numCandidates in $vectorSearch.
const vectorSearchOverrequestFactor = 10;

const searchStage = {
    $search: {
        index: getMovieSearchIndexSpec().name,
        text: {query: "ape", path: ["fullplot", "title"]},
        scoreDetails: true
    }
};

const vectorSearchStage = {
    $vectorSearch: {
        // Get the embedding for 'Tarzan the Ape Man', which has _id = 6.
        queryVector: getMoviePlotEmbeddingById(6),
        path: "plot_embedding",
        numCandidates: limit * vectorSearchOverrequestFactor,
        index: getMovieVectorSearchIndexSpec().name,
        limit: limit,
    }
};

const simpleMatchStage = {
    $match: {
        genres: {$in: ["Action"]},
    }
};

const reverseSortByIdStage = {
    $sort: {_id: -1}
};

const scoreMinMaxScalerNormalizationStage = {
    $score: {
        score: {$multiply: ["$_id", 5]},
        normalization: "minMaxScaler",
        weight: 0.5,
        scoreDetails: true,
    }
};

const scoreSigmoidNormalizationStage = {
    $score: {
        score: {$multiply: ["$_id", 5.1]},
        normalization: "sigmoid",
        weight: 0.4,
        scoreDetails: true,
    }
};

const recursiveScoreStage = {
    $score: {
        score: {$multiply: [{$meta: "score"}, 10]},
        normalization: "none",
        weight: 0.9,
        scoreDetails: true,
    }
};

const createScoreFusionPipeline = (inputPipelines) => {
    return [
        {
            $scoreFusion: {
                input: {
                    pipelines: inputPipelines,
                    normalization: "none",
                },
                scoreDetails: true,
            }
        },
        {$project: {score: {$meta: "score"}, scoreDetails: {$meta: "scoreDetails"}}}
    ];
};

const maxIdMatch = 15;
const minIdMatch = 0;
const maxIdSearch = 6;
const minIdSearch = 1;
const maxIdVectorSearch = 15;
const minIdVectorSearch = 1;

const buildSigmoidScore = (id, mult, weight) => {
    return 1 / (1 + Math.exp(-(id * mult))) * weight;
};

const buildMinMaxScalerScore = (id, mult, min, max, weight) => {
    return ((id * mult) - (min * mult)) / ((max * mult) - (min * mult)) * weight;
};

const buildRecursiveScore = (previousScore, mult, weight) => {
    return previousScore * mult * weight;
};

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document:
 * "score" : 2.0521023273468018,
 * "details" : {
        "value" : 2.0521023273468018,
        "description" : "the value calculated by...",
        "normalization" : "none",
        "combination": {
            "method" : "average"
        },
        "details" : [
            {
                "inputPipelineName" : "search",
                "inputPipelineRawScore" : 1.5521023273468018,
                "weight" : 2,
                "value" : 3.1042046546936035,
                "details" : [ // search's score details
                    {
                        "value" : 1.5521023273468018,
                        "description" : "average of:",
                        "details" : [ {...} ]
                    }
                ]
            },
            {
                "inputPipelineName" : "vector",
                "inputPipelineRawScore" : 1,
                "weight" : 1,
                "value" : 1,
                "details" : [ ]
            }

        ]
    }
 */
const runMultipleScoreTest = (inputPipelines,
                              inputPipeline1Normalization,
                              inputPipeline2Normalization,
                              inputPipeline1ExpectedScore,
                              inputPipeline2ExpectedScore) => {
    const pipeline = createScoreFusionPipeline(inputPipelines);
    const res = coll.aggregate(pipeline).toArray();

    assert.gte(res.length, 1);
    assert(res[0].hasOwnProperty("score"));
    assert(res[0].hasOwnProperty("scoreDetails"));

    // The normalization in $scoreFusion should not be replaced by any of the input pipelines'
    // normalizations, and the $scoreFusion normalization is always 'none'.
    assert.eq(res[0].scoreDetails.normalization, "none");

    // We should have two input pipelines in the order that they were specified.
    assert.eq(res[0].scoreDetails.details.length, 2);
    assert.eq(res[0].scoreDetails.details[0].inputPipelineName, "a");
    assert.eq(res[0].scoreDetails.details[1].inputPipelineName, "b");

    // The input pipelines' normalizations should come from the last score producing stage
    // of each input pipeline.
    assert.eq(res[0].scoreDetails.details[0].details.normalization, inputPipeline1Normalization);
    assert.eq(res[0].scoreDetails.details[1].details.normalization, inputPipeline2Normalization);

    assert.eq(res[0].scoreDetails.details[0].value, inputPipeline1ExpectedScore);
    assert.eq(res[0].scoreDetails.details[1].value, inputPipeline2ExpectedScore);

    // The score of the $scoreFusion stage is the average of the two input pipelines' scores.
    assert.eq(res[0].score, (inputPipeline1ExpectedScore + inputPipeline2ExpectedScore) / 2);
};

runMultipleScoreTest({
    a: [searchStage, scoreSigmoidNormalizationStage],
    b: [vectorSearchStage, scoreMinMaxScalerNormalizationStage],
},
                     "sigmoid",
                     "minMaxScaler",
                     buildSigmoidScore(6, 5.1, 0.4),
                     buildMinMaxScalerScore(6, 5.0, minIdVectorSearch, maxIdVectorSearch, 0.5));

runMultipleScoreTest({
    a: [searchStage, scoreSigmoidNormalizationStage, scoreMinMaxScalerNormalizationStage],
    b: [vectorSearchStage, scoreMinMaxScalerNormalizationStage, scoreSigmoidNormalizationStage],
},
                     "minMaxScaler",
                     "sigmoid",
                     buildMinMaxScalerScore(6, 5.0, minIdSearch, maxIdSearch, 0.5),
                     buildSigmoidScore(6, 5.1, 0.4));

runMultipleScoreTest({
    a: [
        simpleMatchStage,
        reverseSortByIdStage,
        scoreSigmoidNormalizationStage,
        scoreMinMaxScalerNormalizationStage
    ],
    b: [
        simpleMatchStage,
        reverseSortByIdStage,
        scoreMinMaxScalerNormalizationStage,
        scoreSigmoidNormalizationStage
    ],
},
                     "minMaxScaler",
                     "sigmoid",
                     buildMinMaxScalerScore(15, 5.0, minIdMatch, maxIdMatch, 0.5),
                     buildSigmoidScore(15, 5.1, 0.4));

runMultipleScoreTest(
    {
        a: [searchStage, scoreSigmoidNormalizationStage, recursiveScoreStage],
        b: [vectorSearchStage, scoreMinMaxScalerNormalizationStage, recursiveScoreStage],
    },
    "none",
    "none",
    buildRecursiveScore(buildSigmoidScore(6, 5.1, 0.4), 10.0, 0.9),
    buildRecursiveScore(
        buildMinMaxScalerScore(6, 5.0, minIdVectorSearch, maxIdVectorSearch, 0.5), 10.0, 0.9));

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
