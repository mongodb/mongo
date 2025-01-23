/*
 * Tests applying weights to $rankFusion. Uses the same input pipelines as ranked_fusion_test.js,
 * but with different weighting to produce different result ordering.
 * Also tests that proper error codes are thrown for specifying bad pipeline weights.
 * @tags: [ featureFlagRankFusionFull, requires_fcv_81 ]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e/lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
} from "jstests/with_mongot/e2e/lib/search_e2e_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

createSearchIndex(coll, getMovieSearchIndexSpec());
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

// Builds a $rankFusion query with custom weights object.
function buildRankFusionQueryWithWeights(weights) {
    const limit = 20;
    // Multiplication factor of limit for numCandidates in $vectorSearch.
    const vectorSearchOverrequestFactor = 10;
    const query = [
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
                },
                combination: {weights}
            },
        },
        {$limit: limit}
    ];
    return query;
}

// Asserts documents resulting from $rankFusion query are in expected order.
function runTest(weights, expectedResultIds) {
    let query = buildRankFusionQueryWithWeights(weights);
    let results = coll.aggregate(query).toArray();
    assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.MOVIES), results);
}

// Asserts the $rankFusion query fails with the expected error code, given a bad weights object.
function runTestExpectError(weights, errorCode) {
    let query = buildRankFusionQueryWithWeights(weights);
    assert.throwsWithCode(function() {
        coll.aggregate(query);
    }, errorCode);
}

// Although you can't easily tell from the list of IDs, when cross-referencing with the data set, it
// is clear that the weights are applied properly if these result set orderings are returned.
runTest({vector: 1, search: 1}, [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15]);
runTest({vector: 0, search: 1}, [6, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15]);
runTest({vector: 1, search: 0}, [6, 4, 8, 9, 10, 12, 13, 5, 1, 14, 3, 2, 11, 7, 15]);
runTest({vector: 5, search: 3}, [6, 4, 1, 5, 3, 2, 8, 9, 10, 12, 13, 14, 11, 7, 15]);
// Specifying a subset of weights is also valid.
runTest({vector: 1}, [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15]);
runTest({search: 0}, [6, 4, 8, 9, 10, 12, 13, 5, 1, 14, 3, 2, 11, 7, 15]);
// No specified weights defaults to 1 for all pipelines.
runTest({}, [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15]);

// Now test that improperly specified weights fail as expected.
// More weights than pipelines.
runTestExpectError({vector: 0.1, search: 0.2, a: 0.3}, 9460301);
// Single non-existant pipeline.
runTestExpectError({a: 0.1}, 9967400);
// One existant, and one non-existant pipeline.
runTestExpectError({vector: 0.1, a: 0.2}, 9967400);
// Non-numeric weight
runTestExpectError({vector: 0.1, search: "0.2"}, 13118);
// Negative weight
runTestExpectError({vector: 0.1, search: -0.2}, 9460300);

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
