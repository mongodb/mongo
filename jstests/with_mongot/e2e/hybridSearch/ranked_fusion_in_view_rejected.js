/**
 * Tests that $rankFusion in a view definition is always rejected.
 *
 * TODO SERVER-101721 Enable $rankFusion to be run in a view definition.
 *
 * @tags: [featureFlagRankFusionBasic, requires_fcv_81]
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

function runTest(underlyingCollOrViewName) {
    const rankFusionViewName = "rank_fusion_view";

    // Check that it fails both with a $rankFusion with mongot stages and a $rankFusion without
    // mongot stages.
    assert.commandFailedWithCode(
        db.createView(
            rankFusionViewName, underlyingCollOrViewName, rankFusionPipelineWithoutSearch),
        ErrorCodes.OptionNotSupportedOnView);
    assert.commandFailedWithCode(
        db.createView(rankFusionViewName, underlyingCollOrViewName, rankFusionPipelineWithSearch),
        ErrorCodes.OptionNotSupportedOnView);
}

// First test that creating a view with $rankFusion on the collection is rejected.
runTest(collName);

// Then test that a $rankFusion view on top of a nested identity view is also rejected.
const identityViewName = jsTestName() + "_identity_view";
assert.commandWorked(db.createView(identityViewName, collName, []));
runTest(identityViewName);

// Lastly test that a $rankFusion view on top of a nested non-identity view is also rejected.
const viewName = jsTestName() + "_view";
assert.commandWorked(db.createView(viewName, collName, [{$match: {genres: "Fantasy"}}]));
runTest(viewName);

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
