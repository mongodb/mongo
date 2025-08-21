/*
 * Tests that $scoreFusion must not be any stage other than the first in an aggregation pipeline.
 *
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_82 ]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec,
} from "jstests/with_mongot/e2e_lib/data/movies.js";

const collName = "score_fusion";
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(coll.insertMany(getMovieData()));

createSearchIndex(coll, getMovieSearchIndexSpec());
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

const limit = 20;
const vectorSearchOverrequestFactor = 10;

const ScoreFusionMustBeFirstStageOfPipelineErrCode = 10170100;

let limitStage = {$limit: limit};

let matchStage = {
    $match: {
        number_of_reviews: {
            $gte: 25,
        },
    },
};

let scoreFusionWithoutSearchStage = {
    $scoreFusion: {
        input: {
            pipelines: {
                a: [{$sort: {_id: 1}}, {$score: {score: "$number_of_reviews", normalization: "none"}}],
            },
            normalization: "none",
        },
    },
};

let scoreFusionWithSearchStage = {
    $scoreFusion: {
        input: {
            pipelines: {
                vector: [
                    {
                        $vectorSearch: {
                            // Get the embedding for 'Tarzan the Ape Man', which has _id = 6.
                            queryVector: getMoviePlotEmbeddingById(6),
                            path: "plot_embedding",
                            numCandidates: limit * vectorSearchOverrequestFactor,
                            index: getMovieVectorSearchIndexSpec().name,
                            limit: limit,
                        },
                    },
                ],
                search: [
                    {
                        $search: {
                            index: getMovieSearchIndexSpec().name,
                            text: {query: "ape", path: ["fullplot", "title"]},
                        },
                    },
                    {$limit: limit},
                ],
            },
            normalization: "none",
        },
    },
};

function assertScoreFusionMustBeFirstStageInPipeline(pipeline) {
    assert.commandFailedWithCode(
        coll.runCommand("aggregate", {pipeline: pipeline, cursor: {}}),
        ScoreFusionMustBeFirstStageOfPipelineErrCode,
    );
}

// Simple case where a typical search-based $scoreFusion is preceded by a filter (match stage).
// The desugared output is an invalid pipeline anyways since $search would illegally appear not
// as the first stage, but we should first identify the error that $scoreFusion itself
// must be the first stage.
assertScoreFusionMustBeFirstStageInPipeline([matchStage, scoreFusionWithSearchStage, limitStage]);

// In this case where the desugared output of the aggregation would be a valid query,
// but we should still reject because we catch that $scoreFusion is the first stage.
assertScoreFusionMustBeFirstStageInPipeline([matchStage, scoreFusionWithoutSearchStage, limitStage]);

// Tests that its not sufficient for $scoreFusion to be the first stage of the pipeline,
// it must also not be any stage other than the first.
// (i.e. if we have 2 different $scoreFusion in a pipeline, even if one is the first,
// the query still fails).
assertScoreFusionMustBeFirstStageInPipeline([
    scoreFusionWithoutSearchStage,
    matchStage,
    scoreFusionWithSearchStage,
    limitStage,
]);

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
