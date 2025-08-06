/**
 * Tests that $rankFusion/$scoreFusion in a view definition is always rejected.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_82]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e_lib/data/movies.js";

const collName = jsTestName();

const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(coll.insertMany(getMovieData()));

createSearchIndex(coll, getMovieSearchIndexSpec());
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

const rankFusionPipelineWithoutSearch =
    [{$rankFusion: {input: {pipelines: {a: [{$sort: {x: 1}}]}}}}];
const scoreFusionPipelineWithoutSearch = [{
    $scoreFusion: {
        input: {
            pipelines: {single: [{$score: {score: "$single", normalization: "minMaxScaler"}}]},
            normalization: "none"
        },
        combination: {method: "avg"}
    }
}];
const limit = 20;
const vectorSearchOverrequestFactor = 10;
const rankFusionPipelineWithSearch = [
    {
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
    },
    {$limit: limit}
];
const scoreFusionPipelineWithSearch = [
    {
        $scoreFusion: {
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
                },
                normalization: "none"
            },
            combination: {method: "avg"}
        }
    },
    {$limit: limit}
];

function assertViewsWithRankFusionOrScoreFusionFail(
    underlyingCollOrViewName, viewName, pipelineWithoutSearch, pipelineWithSearch) {
    // Check that it fails both with a $rankFusion/$scoreFusion with mongot stages and a
    // $rankFusion/$scoreFusion without mongot stages.
    assert.commandFailedWithCode(
        db.createView(viewName, underlyingCollOrViewName, pipelineWithoutSearch),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        db.createView(viewName, underlyingCollOrViewName, pipelineWithSearch),
        ErrorCodes.OptionNotSupportedOnView);
}

function runTest(viewName, pipelineWithoutSearch, pipelineWithSearch) {
    // First test that creating a view with $rankFusion/$scoreFusion on the collection is rejected.
    assertViewsWithRankFusionOrScoreFusionFail(
        collName, viewName, pipelineWithoutSearch, pipelineWithSearch);

    // Then test that a $rankFusion/$scoreFusion view on top of a nested identity view is also
    // rejected.
    const identityViewName = jsTestName() + "_identity_view";
    assert.commandWorked(db.createView(identityViewName, collName, []));
    assertViewsWithRankFusionOrScoreFusionFail(
        identityViewName, viewName, pipelineWithoutSearch, pipelineWithSearch);

    // Lastly test that a $rankFusion/$scoreFusion view on top of a nested non-identity view is also
    // rejected.
    const nestedViewName = jsTestName() + "_view";
    assert.commandWorked(db.createView(nestedViewName, collName, [{$match: {genres: "Fantasy"}}]));
    assertViewsWithRankFusionOrScoreFusionFail(
        nestedViewName, viewName, pipelineWithoutSearch, pipelineWithSearch);
}

runTest("rank_fusion_view", rankFusionPipelineWithoutSearch, rankFusionPipelineWithSearch);
runTest("score_fusion_view", scoreFusionPipelineWithoutSearch, scoreFusionPipelineWithSearch);

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
