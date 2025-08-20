/*
 * Tests that $rankFusion must not be any stage other than the first in an aggregation pipeline.
 *
 * @tags: [ featureFlagRankFusionBasic ]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e/lib/data/movies.js";

const collName = "rank_fusion";
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(coll.insertMany(getMovieData()));

createSearchIndex(coll, getMovieSearchIndexSpec());
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

const limit = 20;
const vectorSearchOverrequestFactor = 10;

const RankFusionMustBeFirstStageOfPipelineErrCode = 10170100;

let limitStage = {$limit: limit};

let matchStage = {
    $match: {
        number_of_reviews: {
            $gte: 25,
        },
    }
};

let rankFusionWithoutSearchStage = {
    $rankFusion: {
        input: {
            pipelines: {
                a: [{$sort: {_id: 1}}],
            }
        }
    },
};

let rankFusionWithSearchStage = {
    $rankFusion: {
        input: {
            pipelines: {
                vector: [{
                    $vectorSearch: {
                        // Get the embedding for 'Tarzan the Ape Man', which has _id = 6.
                        queryVector: getMoviePlotEmbeddingById(6),
                        path: "plot_embedding",
                        numCandidates: limit * vectorSearchOverrequestFactor,
                        index: getMovieVectorSearchIndexSpec().name,
                        limit: limit,
                    }
                }],
                search: [
                    {
                        $search: {
                            index: getMovieSearchIndexSpec().name,
                            text: {query: "ape", path: ["fullplot", "title"]},
                        }
                    },
                    {$limit: limit}
                ]
            }
        }
    },
};

function assertRankFusionMustBeFirstStageInPipeline(pipeline) {
    assert.commandFailedWithCode(coll.runCommand("aggregate", {pipeline: pipeline, cursor: {}}),
                                 RankFusionMustBeFirstStageOfPipelineErrCode);
}

// Simple case where a typical search-based $rankFusion is preceded by a filter (match stage).
// The de-sugared output is an invalid pipeline anyways since $search would illegally appear not
// as the first stage, but we should first identify the error that $rankFusion itself
// must be the first stage.
assertRankFusionMustBeFirstStageInPipeline([matchStage, rankFusionWithSearchStage, limitStage]);

// In this case where the de-sugared output of the aggregation would be a valid query,
// but we should still reject because we catch that $rankFusion is the first stage.
assertRankFusionMustBeFirstStageInPipeline([matchStage, rankFusionWithoutSearchStage, limitStage]);

// Tests that its not sufficient for $rankFusion to be the first stage of the pipeline,
// it must also not be any stage other than the first.
// (i.e. if we have 2 different $rankFusion in a pipeline, even if one is the first,
// the query still fails).
assertRankFusionMustBeFirstStageInPipeline(
    [rankFusionWithoutSearchStage, matchStage, rankFusionWithSearchStage, limitStage]);

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
